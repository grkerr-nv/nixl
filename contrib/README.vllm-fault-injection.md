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
from the same build. Run these from the repository root in a build environment
matching the vLLM image's Python, CUDA, architecture, and system libraries:

```bash
BUILD=/opt/nixl-build
WHEELS=/opt/nixl-wheels
UCX=/opt/ucx
PLAT=manylinux_2_39_$(uname -m)

# Name the distribution for the CUDA major version of the vLLM image. The image
# derives the wheel's library directory from this name.
python3 contrib/tomlutil.py --wheel-name nixl-cu13 pyproject.toml

# A release build keeps the proxy on the dynamic discovery path, and an empty
# static_plugins keeps UCX off the built-in one. Keep the build tree: the repair
# step and the meta wheel both come out of it.
uv build --wheel --no-build-isolation --out-dir "$WHEELS" -Cbuild-dir="$BUILD" \
  -Csetup-args=-Dbuildtype=release \
  -Csetup-args=-Dbuild_fault_injection=true \
  -Csetup-args=-Denable_plugins=UCX \
  -Csetup-args=-Dstatic_plugins= \
  -Csetup-args=-Ducx_path="$UCX"

# Repair into an empty directory. Pin the platform tag, as build-wheel.sh does,
# so it follows the vLLM image's glibc instead of the builder's. The telemetry
# exporter links prometheus-cpp by a SONAME that only the build tree carries, so
# put that on the search path or repair fails on libcore.
rm -rf dist && mkdir dist
LD_LIBRARY_PATH="$BUILD/subprojects/prometheus-cpp:${LD_LIBRARY_PATH:-}" \
  auditwheel repair --plat "$PLAT" \
    --exclude 'libcuda*' --exclude 'libcufile*' --exclude 'libcuobjclient*' \
    --exclude 'libssl*' --exclude 'libcrypto*' --exclude 'libefa*' \
    --exclude 'libhwloc*' --exclude 'libfabric*' --exclude 'libtorch*' \
    --exclude 'libc10*' --exclude 'libdoca*' --exclude 'libred_client*' \
    --exclude 'libred_async*' --exclude 'liblz4*' \
    "$WHEELS"/*.whl --wheel-dir dist

# Bundle the UCX transport modules, which are dlopened and so invisible to
# auditwheel. --skip-nixl-plugins because the NIXL plugins are already in the
# wheel; without it the script looks for a separate C++ install and fails.
contrib/wheel_add_ucx_plugins.py --ucx-plugins-dir "$UCX/lib/ucx" \
  --skip-nixl-plugins dist/*.whl

cp "$BUILD"/src/bindings/python/nixl-meta/*.whl dist/
```

`dist` is the build context for `Dockerfile.vllm`, which installs every wheel it
finds. Keep exactly the repaired CUDA wheel and the `nixl` meta wheel there: a
failed `auditwheel repair` leaves the unrepaired wheel beside the repaired one,
and installing both is silently wrong.

The image expects this repository's Meson Python wheel layout. It checks that
both plugin libraries exist and can load, and fails if either check fails.
Stock NIXL wheels omit the fault proxy. A debug build or statically linked UCX
can bypass the proxy; use the options above.

From the repository root:

```bash
docker build -f contrib/Dockerfile.vllm \
  --build-arg BASE_IMAGE="vllm/vllm-openai:<version>" \
  -t nixl-vllm:latest /path/to/wheels
docker build -t nixl-vllm-fault:latest - \
  < contrib/Dockerfile.vllm-fault-injection
```

Pin the vLLM version for an experiment. Override `BASE_IMAGE` on the second build
if the first image uses a different tag; `--build-arg` works the same when the
Dockerfile is read from stdin.

The second build takes the Dockerfile on stdin so that its context stays empty.
It copies nothing, and passing `.` instead would upload the whole repository —
2.9 GB in a checkout with build directories, since there is no `.dockerignore`.

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
Confirm the fault-proxy initialization record appears in each worker's logs:

```bash
docker logs <container> 2>&1 | grep 'event=nixl_fault_'
```

Startup emits one `event=nixl_fault_config` record per proxy, carrying the
`experiment_id`, `config_digest`, `real_plugin_path` and `rule_count`, followed
by one `event=nixl_fault_rule` record per configured rule. A rule that fires
emits `event=nixl_fault_injection` with the `rule_id`, `method`, `operation`,
`occurrence` and `status`; `occurrence` echoes the rule's `inject_on_occurrence`
rather than counting calls. `event=nixl_fault_double_claim` is a warning that one
call matched two rules and only one status was returned, which means the run is
not measuring what the configuration describes.

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

Arming both endpoints this way is a choice, not a requirement. A rule fires only
where its call is made, and `postXfer` is evaluated on the side that posts the
transfer, so in a read-pull deployment the prefiller's copy stays armed but idle
— which inverts as soon as the rule moves to `NIXL_WRITE`. To scope a fault to
one side, add the two flags to that endpoint only and leave the other on the
image's inert default: it still loads the proxy, still forwards to genuine UCX,
and still logs its initialization record, so it serves as a control. Its records
carry the inert `experiment_id` and a different `config_digest`, which is how to
tell the two sides apart in a shared log directory. That is what most
single-fault experiments want.

The rule fails the first matching read submitted by each initialized proxy.
Choose `NIXL_WRITE` if the tested connector uses writes. A successful request
alone does not prove the rule fired; the `event=nixl_fault_injection` record is
the proof. Each worker counts independently.
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
