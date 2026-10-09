"""Tiny Llama-2 style transformer that matches llama2.c run.c's forward()
exactly (RMSNorm eps 1e-5, interleaved-pair RoPE, SwiGLU, shared classifier),
plus the llama2.c legacy (version 0) checkpoint writer.
"""
import struct
from dataclasses import dataclass

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F


@dataclass
class Cfg:
    dim: int = 48
    hidden_dim: int = 96
    n_layers: int = 2
    n_heads: int = 4
    vocab_size: int = 108
    seq_len: int = 48
    dropout: float = 0.05


class RMSNorm(nn.Module):
    def __init__(self, d):
        super().__init__()
        self.weight = nn.Parameter(torch.ones(d))

    def forward(self, x):
        return x * torch.rsqrt(x.pow(2).mean(-1, keepdim=True) + 1e-5) * self.weight


def rope_tables(head_size, seq_len):
    j = torch.arange(0, head_size, 2).float()
    freqs = 1.0 / (10000.0 ** (j / head_size))
    t = torch.arange(seq_len).float()
    ang = torch.outer(t, freqs)
    return torch.cos(ang), torch.sin(ang)     # (seq, hs/2)


def apply_rope(x, cos, sin):
    # x: (B, T, H, hs) ; pairs (2i, 2i+1)
    x0, x1 = x[..., 0::2], x[..., 1::2]
    c = cos[None, :, None, :]
    s = sin[None, :, None, :]
    out = torch.stack([x0 * c - x1 * s, x0 * s + x1 * c], dim=-1)
    return out.flatten(-2)


class Block(nn.Module):
    def __init__(self, c: Cfg):
        super().__init__()
        self.c = c
        d = c.dim
        self.wq = nn.Linear(d, d, bias=False)
        self.wk = nn.Linear(d, d, bias=False)
        self.wv = nn.Linear(d, d, bias=False)
        self.wo = nn.Linear(d, d, bias=False)
        self.w1 = nn.Linear(d, c.hidden_dim, bias=False)
        self.w2 = nn.Linear(c.hidden_dim, d, bias=False)
        self.w3 = nn.Linear(d, c.hidden_dim, bias=False)
        self.attention_norm = RMSNorm(d)
        self.ffn_norm = RMSNorm(d)
        self.drop = nn.Dropout(c.dropout)

    def forward(self, x, cos, sin, mask):
        B, Tn, D = x.shape
        H = self.c.n_heads
        hs = D // H
        h = self.attention_norm(x)
        q = apply_rope(self.wq(h).view(B, Tn, H, hs), cos, sin)
        k = apply_rope(self.wk(h).view(B, Tn, H, hs), cos, sin)
        v = self.wv(h).view(B, Tn, H, hs)
        q, k, v = (t.transpose(1, 2) for t in (q, k, v))
        att = (q @ k.transpose(-2, -1)) / (hs ** 0.5)
        att = att.masked_fill(mask[:, :, :Tn, :Tn] == 0, float("-inf"))
        att = self.drop(F.softmax(att, dim=-1))
        y = (att @ v).transpose(1, 2).reshape(B, Tn, D)
        x = x + self.drop(self.wo(y))
        h = self.ffn_norm(x)
        x = x + self.drop(self.w2(F.silu(self.w1(h)) * self.w3(h)))
        return x


class MotionLM(nn.Module):
    def __init__(self, c: Cfg):
        super().__init__()
        self.c = c
        self.tok_embeddings = nn.Embedding(c.vocab_size, c.dim)
        self.layers = nn.ModuleList([Block(c) for _ in range(c.n_layers)])
        self.norm = RMSNorm(c.dim)
        cos, sin = rope_tables(c.dim // c.n_heads, c.seq_len)
        self.register_buffer("cos", cos, persistent=False)
        self.register_buffer("sin", sin, persistent=False)
        m = torch.tril(torch.ones(c.seq_len, c.seq_len)).view(1, 1, c.seq_len, c.seq_len)
        self.register_buffer("mask", m, persistent=False)
        self.apply(self._init)
        for n, p in self.named_parameters():
            if n.endswith("wo.weight") or n.endswith("w2.weight"):
                nn.init.normal_(p, 0.0, 0.02 / (2 * c.n_layers) ** 0.5)

    @staticmethod
    def _init(m):
        if isinstance(m, (nn.Linear, nn.Embedding)):
            nn.init.normal_(m.weight, 0.0, 0.02)

    def forward(self, idx):
        Tn = idx.shape[1]
        x = self.tok_embeddings(idx)
        for L in self.layers:
            x = L(x, self.cos[:Tn], self.sin[:Tn], self.mask)
        x = self.norm(x)
        return x @ self.tok_embeddings.weight.t()

    def n_params(self):
        return sum(p.numel() for p in self.parameters())


def export_llama2c(model: MotionLM, path):
    """llama2.c legacy format: 7 x int32 header, then fp32 weights in run.c's
    memory_map_weights() order (vocab_size > 0 = shared classifier)."""
    c = model.c
    hs = c.dim // c.n_heads

    def w(t):
        return t.detach().cpu().float().numpy().astype("<f4").tobytes()

    L = model.layers
    with open(path, "wb") as f:
        f.write(struct.pack("<7i", c.dim, c.hidden_dim, c.n_layers, c.n_heads,
                            c.n_heads, c.vocab_size, c.seq_len))
        f.write(w(model.tok_embeddings.weight))
        for name in ["attention_norm", "wq", "wk", "wv", "wo", "ffn_norm", "w1", "w2", "w3"]:
            for l in L:
                f.write(w(getattr(l, name).weight))
        f.write(w(model.norm.weight))
        f.write(np.zeros(c.seq_len * hs // 2, "<f4").tobytes())   # freq_cis_real (unused)
        f.write(np.zeros(c.seq_len * hs // 2, "<f4").tobytes())   # freq_cis_imag (unused)
