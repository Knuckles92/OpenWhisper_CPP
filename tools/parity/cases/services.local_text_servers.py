"""Curated inputs for services/local_text_servers.py (fv_llm llm/local_servers.h).

Each server case names a fixture of tests/fixtures/llm/servers/: a table of canned answers by
"METHOD /path" (Ollama's /api/show answers by the model in the request body; any other route is
a 404 "Not Found", as tests/test_local_text_servers.py's fakes answer). The module's _http is
replaced by an httpx client over that table, so discover_chat_models and identify_server run
their real code with no network. discover records the chat models, the specs remembered for the
base URL (TextModelSpec.to_dict), and the distinct requests made (method, path, whether a Bearer
key was carried); a failure is recorded as {"raises": class, "message": str(exc)}. The pure helpers
run as they are. The C++ replay is tests/llm/test_local_servers.cpp.
"""
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import ow_isolation  # noqa: E402

FIXTURES = ow_isolation.REPO / "tests" / "fixtures" / "llm" / "servers"
BASE = "http://127.0.0.1:8080/v1"


def _fixture(name):
    return json.loads((FIXTURES / f"{name}.json").read_text(encoding="utf-8"))


def _answer(route, request):
    import httpx

    if "json_by_model" in route:
        model = json.loads(request.content)["model"]
        return httpx.Response(route["status"], json=route["json_by_model"][model])
    if "json" in route:
        return httpx.Response(route["status"], json=route["json"])
    return httpx.Response(route["status"], text=route["text"])


def _install(module, name):
    import httpx

    routes = _fixture(name)["routes"]
    seen = []

    def handler(request):
        seen.append([request.method, request.url.path,
                     request.headers.get("authorization", "").startswith("Bearer ")])
        route = routes.get(f"{request.method} {request.url.path}")
        if route is None:
            return httpx.Response(404, text="Not Found")
        return _answer(route, request)

    def http(base_url, api_key, timeout):
        headers = {"Authorization": f"Bearer {api_key}"} if api_key else {}
        return httpx.Client(base_url=module.server_root(base_url), headers=headers,
                            transport=httpx.MockTransport(handler), follow_redirects=False)

    original = module._http
    module._http = http
    return seen, lambda: setattr(module, "_http", original)


def _distinct(seen):
    """The distinct requests, sorted. Ollama's /api/show calls run on a thread pool whose
    executor.map cancels the calls not yet started once one fails, so how many of them happen
    depends on thread timing; which kinds of request were made does not."""
    return sorted({tuple(request) for request in seen})


def _discover(module, name, base_url, server, api_key):
    from services import text_model_catalog

    seen, restore = _install(module, name)
    text_model_catalog._server_specs.clear()
    try:
        models = module.discover_chat_models(base_url, server, api_key)
    except Exception as exc:  # noqa: BLE001 - the vector records what the caller sees
        return {"raises": type(exc).__name__, "message": str(exc), "requests": _distinct(seen)}
    finally:
        restore()
    specs = text_model_catalog._server_specs.get(base_url, {})
    return {"models": models, "specs": {m: s.to_dict() for m, s in specs.items()},
            "requests": _distinct(seen)}


def _identify_server(module, name, base_url):
    seen, restore = _install(module, name)
    try:
        return {"server": module.identify_server(base_url), "requests": seen}
    except Exception as exc:  # noqa: BLE001
        return {"raises": type(exc).__name__, "message": str(exc), "requests": seen}
    finally:
        restore()


def _spec(module, context, tools, reasoning, effort):
    return module._spec(context, tools=tools, reasoning=reasoning, effort=effort).to_dict()


def _by_name(module, model):
    spec = module._by_name(model)
    return None if spec is None else spec.to_dict()


FIXTURE_NAMES = sorted(p.stem for p in FIXTURES.glob("*.json"))
PRESETS = {"vllm": "vllm", "vllm_keyed": "vllm", "llama_cpp": "llama_cpp",
           "llama_cpp_router": "llama_cpp", "lm_studio": "lm_studio",
           "lm_studio_reasoning": "lm_studio", "lm_studio_v0": "lm_studio", "localai": "localai",
           "localai_no_capabilities": "localai", "ollama": "ollama", "ollama_odd": "ollama"}

WRAPPERS = {
    "discover_chat_models": _discover,
    "identify_server": _identify_server,
    "_spec": _spec,
    "_by_name": _by_name,
}

CASES = {
    "discover_chat_models": (
        [[name, BASE, None, None] for name in FIXTURE_NAMES]
        + [[name, BASE, server, None] for name, server in sorted(PRESETS.items())]
        + [["vllm", BASE, "vllm", "sk-local"], ["ollama", "http://127.0.0.1:8080/v1/", "ollama", None],
           ["lm_studio", BASE, "ollama", None], ["localai", BASE, "vllm", None]]
        # A custom endpoint started with a key: its identification probes carry the key too.
        + [["lm_studio", BASE, None, "sk-local"], ["ollama", BASE, None, "sk-local"],
           ["llama_cpp_keyed", BASE, None, "sk-local"]]
    ),
    "identify_server": [[name, BASE] for name in FIXTURE_NAMES],
    "server_root": [[url] for url in [
        "http://127.0.0.1:8080/v1", "http://127.0.0.1:8080/v1/", "http://127.0.0.1:8080",
        "http://host:1/prefix/v1", "http://host:1/prefix/v1/", "http://host:1/v1x",
        "https://h/v1?x=1#f", "http://h/v1/v1", "http://[::1]:9/v1",
    ]],
    "_spec": [
        [0, True, False, None], [8192, True, False, {"off": "none"}], [100, False, True, None],
        [5000000, True, False, {}], [8191, True, True, {"off": None, "low": "low"}], [-5, True, False, None],
        [1, True, False, None], [9000, False, False, {"off": "none"}],
    ],
    "_by_name": [[m] for m in [
        "qwen3", "text-embedding-3", "EMBED", "bge-reranker", "whisper-1", "tts", "tts-1",
        "piper_tts.onnx", "my/tts", "attsx", "kokoro", "ttsx", "x-tts", "x-tts-y", "Whisper",
    ]],
}

PINS = {
    "fixtures": "tests/fixtures/llm/servers/<name>.json answer every request; unknown routes 404",
    "discover": "_server_specs is cleared before each discover case",
}
