<!--
SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
SPDX-License-Identifier: Apache-2.0
-->

# vLLM with fault injection

`Dockerfile.vllm-fault-injection` extends the image from `Dockerfile.vllm`.
It selects the fault proxy as UCX and starts with an inert configuration.
It points `NIXL_PLUGIN_DIR` at `/opt/nixl-fault/plugins`, a symlink to the
installed proxy directory, and keeps the base image's entrypoint and arguments.
Only `UCX` resolves inside this image; the other NIXL backends are present in the
wheel but not discoverable. Keep the normal `NixlConnector` configuration.

## Build

Build the NIXL wheels from this branch, with the proxy and genuine UCX backend
from the same build. Add these Meson options to the wheel build:

```text
-Dbuildtype=release
-Dbuild_fault_injection=true
-Denable_plugins=UCX
-Dstatic_plugins=
```

For `uv build`, pass each option as `-Csetup-args=<option>`. Use the existing
wheel repair and UCX bundling steps for a portable wheel. Match the vLLM image's
Python, CUDA, architecture, and system-library requirements. Include the matching
`nixl` meta wheel alongside the CUDA wheel in the wheel directory.

The image expects this repository's Meson Python wheel layout. It checks that
both plugin libraries exist and can load, and fails if either check fails.
Stock NIXL wheels omit the fault proxy. A debug build or statically linked UCX
can bypass the proxy; use the options above.

From the repository root:

```bash
docker build -f contrib/Dockerfile.vllm \
  --build-arg BASE_IMAGE="vllm/vllm-openai:<version>" \
  -t nixl-vllm:latest /path/to/wheels
docker build -f contrib/Dockerfile.vllm-fault-injection \
  -t nixl-vllm-fault:latest .
```

Pin the vLLM version for an experiment. Override `BASE_IMAGE` on the second build
if the first image uses a different tag.

Build with BuildKit, the default on Docker 23 and newer. The second Dockerfile
uses a heredoc, which the legacy builder (`DOCKER_BUILDKIT=0`, and older
podman/buildah) skips without reporting anything: the image builds green and has no plugins at
run time. The Dockerfile checks for this and fails the build rather than let it
through, so a `FATAL: the setup step did not run` message means the builder, not
the wheels.

## Run

First run the existing prefill/decode deployment with `nixl-vllm:latest` to check
the direct baseline. Then use `nixl-vllm-fault:latest` on both endpoints to check
inert forwarding. Keep the same model, network, GPU, and connector arguments.
Confirm the fault-proxy initialization record appears in each worker's logs.

If restricting `UCX_TLS` on a GPU host, include CUDA support, for example
`tcp,self,cuda_copy`. NIXL validates this even when vLLM uses CPU-staged KV buffers.

Export the inert configuration so the genuine plugin path matches the wheel:

```bash
docker run --rm --entrypoint cat nixl-vllm-fault:latest \
  /opt/nixl-fault/inert.toml > fault.toml
```

Change `experiment_id` to `first-read-vllm`. Replace `rules = []` with:

```toml
[[fault_injection.rules]]
id = "first-read"
method = "postXfer"
operation = "NIXL_READ"
inject_on_occurrence = 1
status = "NIXL_ERR_BACKEND"
```

Add these flags before the image name in each endpoint's existing `docker run`
command, and use `nixl-vllm-fault:latest`:

```bash
--mount type=bind,src="$PWD/fault.toml",dst=/etc/nixl/fault.toml,readonly \
-e NIXL_FAULT_CONFIG_FILE=/etc/nixl/fault.toml
```

The rule fails the first matching read submitted by each initialized proxy.
Choose `NIXL_WRITE` if the tested connector uses writes. Check the injection log
for the experiment, rule, method, operation, and occurrence; a successful request
alone does not prove the rule fired. Each worker counts independently.
Restart workers after changing the configuration. The file is read once.
`NIXL_PLUGIN_DIR` is set in the image, so an overridden entrypoint, a replaced
command, and `docker exec` all still select the proxy. Setting `NIXL_PLUGIN_DIR`
from outside overrides that selection silently, leaving the genuine backend in
place; the initialization record is the only confirmation that the proxy loaded.
See the [harness guide](../src/plugins/fault_injection/README.md) for rule semantics
and limitations. This covers host-agent transfers, not NIXL-EP device transfers.

## Toy proxy

The inherited `/usr/local/bin/toy_proxy_server.py` is vLLM's HTTP router for
disaggregated prefill/decode tests. It sends a request to prefill, passes the
returned KV-transfer metadata to decode, and streams the decoder's response.
NIXL transfers the KV data between workers. The fault proxy intercepts NIXL
backend calls inside those workers.

Run the HTTP router separately, overriding the inherited vLLM entrypoint:

```bash
docker run --rm --network host --entrypoint python3 nixl-vllm:latest \
  /usr/local/bin/toy_proxy_server.py --host 0.0.0.0 --port 8000 \
  --prefiller-host PREFILL_HOST --prefiller-port 8100 \
  --decoder-host DECODE_HOST --decoder-port 8200
```

The router script is pinned by `Dockerfile.vllm`; verify its protocol against the
chosen vLLM version. Bound client request timeouts when testing injected failures.
