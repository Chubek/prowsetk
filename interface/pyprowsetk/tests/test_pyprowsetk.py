import pyprowsetk as pk
import gc

def test_browser_session():
    b = pk.create_browser()
    s = b.create_session()
    s.load_html('<html><title>Test</title><body><a href="/a">link</a></body></html>', "https://example.com")
    assert s.document().title() == "Test"
    assert len(s.document().query_selector_all("a")) == 1
    s.close()
    del s
    del b
    gc.collect()

def test_memory_network_client():
    b = pk.create_browser()
    c = pk.MemoryNetworkClient()
    r = pk.HttpResponse()
    r.status = 200
    r.body = '<html><title>Nav</title></html>'
    c.set_response("https://example.com/", r)
    b.set_network_client(c)
    s = b.create_session()
    s.navigate("https://example.com/")
    assert s.document().title() == "Nav"
    s.close()
    del s
    del b
    del c
    gc.collect()

def test_xpath_and_ir():
    doc = pk.open_html('<html><body><div class="x">hello</div></body></html>')
    xv = pk.evaluate_xpath(doc, "//div[@class='x']")
    assert len(xv.nodes) == 1
    assert xv.nodes[0].text().strip() == "hello"
    xas = pk.emit_prowse_xas(doc)
    assert len(xas) > 0
    dom = pk.emit_prowse_dom(doc)
    assert len(dom) > 0
    vtd = pk.emit_prowse_vtd(doc)
    decoded = pk.decode_prowse_vtd_bytes(vtd)
    assert len(decoded) == len(xas)
    iml = pk.emit_prowse_iml(doc)
    assert "(document" in iml
    iml2 = pk.expand_prowse_iml(iml, lambda name, args: f"(expanded {name})" if name == "test" else "")
    assert "(document" in iml2

def test_extract_tables():
    doc = pk.open_html('<table><tr><th>Name</th><th>Age</th></tr><tr><td>Alice</td><td>30</td></tr><tr><td>Bob</td><td>25</td></tr></table>')
    tables = pk.extract_tables(doc)
    assert len(tables) == 1
    assert tables[0][0]["Name"] == "Alice"
    assert tables[0][1]["Age"] == "25"

def test_document_to_dict():
    doc = pk.open_html('<html><title>T</title><body>hi</body></html>', url="https://example.com")
    d = pk.document_to_dict(doc)
    assert d["title"] == "T"
    assert "hi" in d["text"]

def test_crawl():
    b = pk.create_browser()
    c = pk.MemoryNetworkClient()
    r1 = pk.HttpResponse(); r1.status = 200; r1.body = '<html><title>A</title><a href="/b">b</a></html>'
    c.set_response("https://example.com/", r1)
    r2 = pk.HttpResponse(); r2.status = 200; r2.body = '<html><title>B</title></html>'
    c.set_response("https://example.com/b", r2)
    b.set_network_client(c)
    pages = pk.crawl("https://example.com/", max_depth=1, browser=b)
    assert len(pages) == 2
    titles = {p["title"] for p in pages}
    assert "A" in titles and "B" in titles
    del b
    del c
    gc.collect()

def test_storage():
    b = pk.create_browser()
    s = b.create_session()
    s.local_storage().set("k", "v")
    assert s.local_storage().get("k") == "v"
    assert "k" in s.local_storage()
    assert s.local_storage().keys() == ["k"]
    s.local_storage().clear()
    assert s.local_storage().keys() == []
    s.close()
    del s; del b; gc.collect()

def test_events():
    b = pk.create_browser()
    seen = []
    b.events().subscribe(pk.EventType.BeforeNavigation, lambda ev: seen.append(ev.url))
    c = pk.MemoryNetworkClient()
    r = pk.HttpResponse(); r.status = 200; r.body = '<html><title>X</title></html>'
    c.set_response("https://example.com/", r)
    b.set_network_client(c)
    s = b.create_session()
    s.navigate("https://example.com/")
    assert seen == ["https://example.com/"]
    s.close()
    del s; del b; del c; gc.collect()

def test_python_hooks():
    b = pk.create_browser()
    hooks = pk.python_hooks(b)
    called = []
    @hooks.before_request
    def h(ev):
        called.append(ev.url)
    # trigger via MemoryNetworkClient navigate
    c = pk.MemoryNetworkClient()
    r = pk.HttpResponse(); r.status = 200; r.body = '<html><title>Y</title></html>'
    c.set_response("https://example.com/", r)
    b.set_network_client(c)
    s = b.create_session()
    s.navigate("https://example.com/")
    assert len(called) == 1
    hooks.clear()
    s.close()
    del s; del b; del c; gc.collect()

def test_endpoint_extraction():
    doc = pk.open_html('<html><body><a href="/api/v1/users">users</a><form action="/api/login"></form></body></html>')
    ext = pk.EndpointExtractor()
    res = ext.extract(doc)
    assert isinstance(res.openapi_yaml, str)
    assert "openapi" in res.openapi_yaml.lower()

def test_web_interface():
    wi = pk.WebInterface()
    req = pk.WebRequest()
    req.method = "GET"
    req.path = "/api/health"
    resp = wi.handle(req)
    assert resp.status == 200
    assert "version" in resp.body or "engine" in resp.body

def test_context_managers():
    with pk.create_browser() as b:
        with b.create_session() as s:
            s.load_html('<html><title>CM</title></html>', "https://example.com")
            assert s.document().title() == "CM"
    gc.collect()

def test_element_dict_access():
    doc = pk.open_html('<div id="a" data-x="1">hi</div>')
    el = doc.query_selector("div")
    assert el["id"] == "a"
    assert "data-x" in el
    el["data-x"] = "2"
    assert el["data-x"] == "2"
    assert el.valid()

def test_redaction():
    red = pk.Redactor()
    assert red.is_sensitive_header("Authorization")
    assert red.redact_url("https://example.com?token=secret") == "https://example.com?token=[REDACTED]"

def test_project_config():
    cfg = pk.parse_project_config('[project]\nname="myproj"\nversion="1.0"\n')
    assert cfg.name == "myproj"
    assert cfg.version == "1.0"

def test_capabilities():
    b = pk.create_browser()
    caps = b.capabilities()
    assert caps.has("engine")
    wp = pk.default_web_platform()
    # capabilities is a property (prop_ro), not a method
    assert isinstance(wp.capabilities.all(), list)
