"""FbkNet: the PyTorch twin of src/dsp/NeuralSeparator.

The topology, activation functions and GRU convention must match the C++ engine
exactly; NeuralSeparator.h is the specification and the C++ test suite compares the
engine against an independent reference. If you change anything here, change the
engine and bump the model version in the same commit.

    f[64] -> tanh(In) -> FiLM(control[4]) -> GRU -> Head[65]
          -> (+ SmootherOut(GRU_s(sigmoid(logits))))  -> sigmoid

Outputs, per frame (one frame = one 128-sample hop):
    [0:32]   voice presence per ERB band
    [32:64]  feedback probability per ERB band
    [64]     broadband voice activity
"""
import torch
from torch import nn

NUM_BANDS = 32
NUM_FEATURES = 2 * NUM_BANDS
NUM_OUTPUTS = 2 * NUM_BANDS + 1
NUM_CONTROLS = 4


class FbkNet(nn.Module):
    def __init__(self, hidden: int = 64, trunk: int = 16, smoother: int = 32):
        super().__init__()
        self.hidden, self.trunk, self.smoother_size = hidden, trunk, smoother
        self.in_ = nn.Linear(NUM_FEATURES, hidden)
        self.film_trunk = nn.Linear(NUM_CONTROLS, trunk)
        self.film_gamma = nn.Linear(trunk, hidden)
        self.film_beta = nn.Linear(trunk, hidden)
        self.core = nn.GRU(hidden, hidden, batch_first=True)
        self.head = nn.Linear(hidden, NUM_OUTPUTS)
        if smoother > 0:
            self.smoother = nn.GRU(NUM_OUTPUTS, smoother, batch_first=True)
            self.smoother_out = nn.Linear(smoother, NUM_OUTPUTS)

    def forward(self, features: torch.Tensor, control: torch.Tensor) -> torch.Tensor:
        """features [B, T, 64], control [B, 4] in [0, 1]. Returns logits [B, T, 65]."""
        t = torch.tanh(self.film_trunk(control))                 # [B, trunk]
        gamma = self.film_gamma(t).unsqueeze(1)                  # [B, 1, H]
        beta = self.film_beta(t).unsqueeze(1)
        x = (1.0 + gamma) * torch.tanh(self.in_(features)) + beta
        h, _ = self.core(x)
        logits = self.head(h)
        if self.smoother_size > 0:
            s, _ = self.smoother(torch.sigmoid(logits))
            logits = logits + self.smoother_out(s)
        return logits
