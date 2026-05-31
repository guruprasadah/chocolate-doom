import math
from operator import call
import socket
import struct
from collections import deque

import cv2
import gymnasium as gym
import numpy as np
from gymnasium import spaces
from stable_baselines3 import PPO
from stable_baselines3.common.monitor import Monitor
from stable_baselines3.common.vec_env import DummyVecEnv
from stable_baselines3.common.callbacks import CheckpointCallback

import torch as th
import torch.nn as nn

from stable_baselines3.common.torch_layers import BaseFeaturesExtractor


# ============================================================
# NETWORK / PROTOCOL
# ============================================================

HOST = "127.0.0.1"
PORT = 6767

HEADER_FORMAT = "<2B10I5f"
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)

OG_FRAMEBUFFER_WIDTH = 320
OG_FRAMEBUFFER_HEIGHT = 200

FRAMEBUFFER_WIDTH = 160
FRAMEBUFFER_HEIGHT = 100

FRAMEBUFFER_COUNT = OG_FRAMEBUFFER_WIDTH * OG_FRAMEBUFFER_HEIGHT
FRAMEBUFFER_BYTES = FRAMEBUFFER_COUNT * 4

TOTAL_PACKET_SIZE = HEADER_SIZE + FRAMEBUFFER_BYTES

INPUT_FORMAT = "<bbbBBB"
INPUT_SIZE = struct.calcsize(INPUT_FORMAT)

HEATMAP_SIZE = 100


# ============================================================
# RL CONTROL CONFIG
# ============================================================

STACK_SIZE = 4

MAX_MOVE = 50
MAX_TURN = 1

DEADZONE = 0.05
BUTTON_THRESHOLD = 0.5


# ============================================================
# SOCKET HELPERS
# ============================================================

def recv_exact(sock: socket.socket, size: int) -> bytes:

    buf = bytearray()

    while len(buf) < size:

        chunk = sock.recv(size - len(buf))

        if not chunk:
            raise ConnectionError("Socket closed")

        buf.extend(chunk)

    return bytes(buf)


# ============================================================
# ACTION HELPERS
# ============================================================

def apply_deadzone(
    value: float,
    threshold: float = DEADZONE
) -> float:

    if abs(value) < threshold:
        return 0.0

    return value


def ternary_confidence_to_int(
    value: float,
    max_magnitude: int
) -> int:

    value = float(np.clip(value, -1.0, 1.0))
    value = apply_deadzone(value)

    if value == 0.0:
        return 0

    sign = 1 if value > 0.0 else -1

    magnitude = int(abs(value) * max_magnitude)

    return sign * magnitude


class DoomCNN(BaseFeaturesExtractor):

    def __init__(self, observation_space, features_dim=512):

        super().__init__(observation_space, features_dim)

        n_input_channels = observation_space.shape[0] # type: ignore

        self.cnn = nn.Sequential(

            nn.Conv2d(n_input_channels, 32, kernel_size=8, stride=4),
            nn.ReLU(),

            nn.Conv2d(32, 64, kernel_size=4, stride=2),
            nn.ReLU(),

            nn.Conv2d(64, 128, kernel_size=3, stride=1),
            nn.ReLU(),

            nn.Conv2d(128, 128, kernel_size=3, stride=1),
            nn.ReLU(),

            nn.Flatten()
        )

        # infer shape
        with th.no_grad():

            sample = th.as_tensor(
                observation_space.sample()[None]
            ).float()

            n_flatten = self.cnn(sample).shape[1]

        self.linear = nn.Sequential(
            nn.Linear(n_flatten, features_dim),
            nn.ReLU()
        )

    def forward(self, observations):

        return self.linear(
            self.cnn(observations)
        )

# ============================================================
# DOOM ENV
# ============================================================

class DoomContinuousEnv(gym.Env):

    metadata = {"render_modes": ["human"]}

    def __init__(self, render: bool = False):

        super().__init__()

        self.render_enabled = render

        # ----------------------------------------------------
        # ACTION SPACE
        #
        # [0] forward/backward
        # [1] strafe left/right
        # [2] turn left/right
        # [3] attack
        # [4] use
        #
        # all normalized to [-1, 1]
        # ----------------------------------------------------

        self.action_space = spaces.Box(
            low=-1.0,
            high=1.0,
            shape=(5,),
            dtype=np.float32
        )

        # ----------------------------------------------------
        # OBSERVATION SPACE
        # ----------------------------------------------------

        self.observation_space = spaces.Box(
            low=0.0,
            high=1.0,
            shape=(
                STACK_SIZE,
                FRAMEBUFFER_HEIGHT,
                FRAMEBUFFER_WIDTH
            ),
            dtype=np.float32
        )

        self.frame_stack = deque(
            maxlen=STACK_SIZE
        )

        self.sock = None

        self.last_ammo = 0
        self.steps_since_damage = 0

        self.episode_steps = 0

        self.heatmap: np.ndarray = np.zeros((HEATMAP_SIZE, HEATMAP_SIZE), dtype=np.float32)

        self.connect()

    # ========================================================
    # CONNECTION
    # ========================================================

    def connect(self):

        print("Waiting for Doom connection...")

        server = socket.socket(
            socket.AF_INET,
            socket.SOCK_STREAM
        )

        server.setsockopt(
            socket.SOL_SOCKET,
            socket.SO_REUSEADDR,
            1
        )

        server.bind((HOST, PORT))
        server.listen(1)

        self.sock, addr = server.accept()

        print(f"Connected to {addr}")

    # ========================================================
    # RECEIVE STATE
    # ========================================================

    def recv_state(self):

        raw = recv_exact(
            self.sock, # type: ignore
            TOTAL_PACKET_SIZE
        )

        header = struct.unpack(
            HEADER_FORMAT,
            raw[:HEADER_SIZE]
        )

        (
            episode_ended,
            dead,
            damage_taken,
            damage_dealt,
            kills,
            ammo_used,
            interactions,
            ammo_picked_up,
            weapons,
            health,
            armor,
            cards,
            x,
            y,
            velocity,
            angle_to_enemy,
            dist_to_enemy
        ) = header

        framebuffer = np.frombuffer(
            raw[HEADER_SIZE:],
            dtype=np.float32
        ).reshape(
            OG_FRAMEBUFFER_HEIGHT,
            OG_FRAMEBUFFER_WIDTH
        )

        framebuffer = cv2.resize(
                            framebuffer,
                            (FRAMEBUFFER_WIDTH, FRAMEBUFFER_HEIGHT),
                            interpolation=cv2.INTER_AREA
                        ).astype(np.float32)

        info = {
            "dead": dead,
            "damage_taken": damage_taken,
            "damage_dealt": damage_dealt,
            "kills": kills,
            "ammo_used": ammo_used,
            "interactions": interactions,
            "ammo_picked_up": ammo_picked_up,
            "weapons": weapons,
            "health": health,
            "armor": armor,
            "cards": cards,
            "x": x,
            "y": y,
            "velocity": velocity,
            "angle_to_enemy": angle_to_enemy,
            "dist_to_enemy": dist_to_enemy,
            "episode_ended": episode_ended
        }

        return framebuffer, info

    # ========================================================
    # FRAME STACK
    # ========================================================

    def get_stacked_obs(self):

        return np.stack(
            self.frame_stack,
            axis=0
        ).astype(np.float32)

    # ========================================================
    # REWARD
    # ========================================================

    def compute_reward(self, info):

        reward = 0.0

        # combat
        reward += info["kills"] * 10.0
        reward += info["damage_dealt"] * 7.5

        # survival penalties
        reward -= info["damage_taken"] * 5

        # ammo waste penalty
        if info["ammo_used"] > 0 and info["damage_dealt"] == 0:
            reward -= info["ammo_used"] * math.fabs(info["angle_to_enemy"]) / 180

        # exploration / progress
        reward += info["interactions"] * 10.0
        reward += info["cards"] * 15.0
        reward += 100 if info["episode_ended"] else 0

        ix = int(info["x"] // 10)
        iy = int(info["y"] // 10)
        self.heatmap[iy, ix] += 0.05
        
        reward -= self.heatmap[iy, ix]

        # self.heatmap *= 0.7

        # exploration motivation
        if info["damage_dealt"] != 0:
            self.steps_since_damage = 0
        else:
            self.steps_since_damage += 1
        reward -= self.steps_since_damage * 0.1

        reward -= info["dist_to_enemy"]

        # death
        if info["dead"]:
            reward -= 100.0

        return reward

    # ========================================================
    # STEP
    # ========================================================

    def step(self, action):

        # ----------------------------------------------------
        # NORMALIZED NN OUTPUTS -> DOOM INPUTS
        # ----------------------------------------------------

        forwardmove = ternary_confidence_to_int(
            action[0],
            MAX_MOVE
        )

        sidemove = ternary_confidence_to_int(
            action[1],
            MAX_MOVE
        )

        # doom engine handles turn acceleration internally
        angleturn = ternary_confidence_to_int(
            action[2],
            MAX_TURN
        )

        attack = (
            1
            if action[3] > BUTTON_THRESHOLD
            else 0
        )

        use = (
            1
            if action[4] > BUTTON_THRESHOLD
            else 0
        )

        restart = 0

        packet = struct.pack(
            INPUT_FORMAT,
            forwardmove,
            sidemove,
            angleturn,
            attack,
            use,
            restart
        )

        self.sock.sendall(packet) # type: ignore

        frame, info = self.recv_state()

        self.frame_stack.append(frame)

        obs = self.get_stacked_obs()

        reward = self.compute_reward(info)

        terminated = bool(info["dead"])
        truncated = bool(info["episode_ended"])

        if self.render_enabled:

            display = obs[-1]

            cv2.imshow(
                "doom",
                display
            )

            cv2.waitKey(1)

        self.episode_steps += 1

        return (
            obs,
            reward,
            terminated,
            truncated,
            info
        )

    # ========================================================
    # RESET
    # ========================================================

    def reset(self, seed=None, options=None):

        super().reset(seed=seed)

        restart_packet = struct.pack(
            INPUT_FORMAT,
            0,
            0,
            0,
            0,
            0,
            1
        )

        self.sock.sendall(restart_packet) # type: ignore

        frame, info = self.recv_state()

        self.frame_stack.clear()

        for _ in range(STACK_SIZE):
            self.frame_stack.append(frame)

        obs = self.get_stacked_obs()

        self.episode_steps = 0

        self.steps_since_damage = 0

        self.heatmap = np.zeros((HEATMAP_SIZE, HEATMAP_SIZE), dtype=np.float32)

        return obs, info

    # ========================================================
    # CLOSE
    # ========================================================

    def close(self):

        if self.sock:
            self.sock.close()

        cv2.destroyAllWindows()


# ============================================================
# ENV FACTORY
# ============================================================

def make_env():

    return Monitor(
        DoomContinuousEnv(render=False)
    )


# ============================================================
# TRAINING
# ============================================================

env = DummyVecEnv([
    make_env
])

model = PPO(
    policy="CnnPolicy",
    env=env,

    learning_rate=3e-4,

    n_steps=2048,
    batch_size=64,
    n_epochs=10,

    gamma=0.99,
    gae_lambda=0.95,
    clip_range=0.2,

    ent_coef=0.01,
    vf_coef=0.5,

    verbose=1,

    tensorboard_log="./doom_tensorboard/",

    policy_kwargs=dict(
        normalize_images=False,
        features_extractor_class=DoomCNN,
        features_extractor_kwargs=dict(
            features_dim=512
        )
    )
)

checkpoint_callback = CheckpointCallback(
    save_freq=100_000,
    save_path="./checkpoints/",
    name_prefix="doom_model"
)

model.learn(
    total_timesteps=10_000_000,
    callback=checkpoint_callback
)

model.save(
    "doom_continuous_ppo"
)