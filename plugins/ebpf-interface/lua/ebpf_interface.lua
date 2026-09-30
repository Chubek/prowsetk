local ebpf = { _version = "0.1.0", _description = "Optional libbpf/eBPF endpoint observation" }
function ebpf.normalize_spec(spec)
  spec = spec or {}
  return { output = spec.output or spec.ebpf_output or "", max_records = tonumber(spec.max_records or 4096) or 4096,
           enabled = spec.enabled ~= false, require_libbpf = spec.require_libbpf == true }
end
function ebpf.capabilities()
  return { "ebpf", "libbpf", "endpoint-observation", "network-tracing", "bpf-object", "bpf-program", "bpf-map", "bpf-link", "ring-buffer", "perf-buffer" }
end
function ebpf.render_observations(endpoints)
  local out = { '{"observations":[' }
  for i, ep in ipairs(endpoints or {}) do
    if i > 1 then out[#out + 1] = ',' end
    local method = tostring(ep.method or "GET"):gsub('\\', '\\\\'):gsub('"', '\\"')
    local url = tostring(ep.url or ep.path or ""):gsub('\\', '\\\\'):gsub('"', '\\"')
    out[#out + 1] = '{"method":"' .. method .. '","url":"' .. url .. '","status":0,"content_type":""}'
  end
  out[#out + 1] = ']}'
  return table.concat(out)
end
return ebpf
