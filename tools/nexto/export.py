# Writes Nexto's weights as one flat float32 file (NXT1 header, then each tensor in a fixed order)
import torch, struct, warnings, numpy as np, sys
warnings.filterwarnings("ignore")
m = torch.jit.load(sys.argv[1])
sd = {n: p.detach().numpy().astype(np.float32) for n, p in m.named_parameters()}
order = []
for pre in ("query_preprocess", "key_value_preprocess"):
    for i in (0, 2): order += [f"net.earl.{pre}.{i}.weight", f"net.earl.{pre}.{i}.bias"]
for b in (0, 1):
    p = f"net.earl.blocks.{b}."
    order += [p + "norm1.weight", p + "norm1.bias", p + "norm2.weight", p + "norm2.bias",
              p + "attention.in_proj_weight", p + "attention.in_proj_bias",
              p + "attention.out_proj.weight", p + "attention.out_proj.bias",
              p + "norm3.weight", p + "norm3.bias",
              p + "linear1.weight", p + "linear1.bias", p + "linear2.weight", p + "linear2.bias"]
for i in (0, 2, 4): order += [f"net.output.net.{i}.weight", f"net.output.net.{i}.bias"]
order += ["net.output.emb_convertor.weight", "net.output.emb_convertor.bias"]
assert set(order) == set(sd), set(sd) ^ set(order)
lut = m.net.output.code_with_constants[1].c0.numpy().astype(np.float32)
with open(sys.argv[2], "wb") as f:
    f.write(b"NXT1"); f.write(struct.pack("<i", len(order) + 1))
    for n in order:
        a = sd[n]; f.write(struct.pack("<i", a.size)); f.write(a.tobytes())
    f.write(struct.pack("<i", lut.size)); f.write(lut.tobytes())
print("wrote", sum(sd[n].size for n in order) + lut.size, "floats")
