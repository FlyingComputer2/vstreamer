# Decoder

Decoders are `component_coder`s ([component_coder.hpp](../src/core/component_coder.hpp)), the
inverse of [encoders](component-encoder.md): push compressed frames with `input`, pull packed NV12
frames with `output`. Keys: [vstreamer.md § Keys](vstreamer.md#keys).

| Class | `input_kind` | `output_kind` | Notes |
|-------|--------------|---------------|-------|
| `jpeg_decoder_multicore` | `MJPEG` | `NV12` | libav worker pool (`workers` 1..8, default 2), results in submit order |
| `h264_decoder_mpp` | `H264` | `NV12` | Rockchip MPP; `cancel_pending_io()` for shutdown |

Rules:

- `input` takes one JPEG or one Annex-B access unit; `output` returns frames in order, `-EAGAIN`
  when none is ready. `pts` and `capture_mono_ns` carry through.
- Output is always **packed** NV12 (no stride padding). Helpers: `src/core/pix_convert.hpp`.
- `output_mode`:
  - `filter` — accept only 4:2:0 sources (MPP `YUV420SP`/`VU`, libav `YUV420P`); other chroma →
    `-ENOTSUP`.
  - `convert` — also accept 4:2:2 (MPP `YUV422SP`/`VU`, libav `YUV422P`) and convert. 8-bit only.
- `output_format` (`format` on query): `nv12` only. Parsers live in `src/core/output_opts.hpp`.
- `h264_decoder_mpp` `output_size_mode`: `stream` sizes output from the SPS, `config` from `size`.
- `jpeg_decoder_multicore` pins workers with `worker_cpu` / `worker_cpus`; `workers` is fixed once
  open (`-EBUSY`).
