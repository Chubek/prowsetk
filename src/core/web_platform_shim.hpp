// The Flatworm web platform shim: a JavaScript bootstrap that builds the
// standard browser globals (document, window, XMLHttpRequest, fetch, timers,
// events, URL, storage, navigator, location) on top of the small set of
// native, host-mediated primitives installed by quickjs_runtime.cpp.
//
// It lives here as a raw string because it is page scripting: JavaScript is
// only ever the page layer (README "JavaScript Execution"), never an
// extension mechanism, and shipping it as data keeps the C++ boundary small.
#ifndef PROWSETK_WEB_PLATFORM_SHIM_HPP
#define PROWSETK_WEB_PLATFORM_SHIM_HPP

namespace prowsetk {

inline constexpr const char kWebPlatformShim[] = R"SHIM(
(function () {
  'use strict';
  var H = globalThis.__prowsetk;
  if (H.__installed) return;
  H.__installed = true;

  var G = globalThis;
  var docState = { readyState: 'loading', fired: {} };

  // ---------- URL helpers ----------
  function parseUrl(href, base) {
    var s = String(href == null ? '' : href);
    if (base == null) {
      var info = H.pageInfo();
      base = info.url || 'about:blank';
    }
    try {
      return H.resolveUrl(s, base);
    } catch (e) {
      return {
        href: s, origin: 'null', protocol: '', host: '', hostname: '',
        port: '', pathname: s, search: '', hash: ''
      };
    }
  }

  function urlParamPairs(search) {
    var pairs = [];
    var q = String(search || '');
    if (q.charAt(0) === '?') q = q.slice(1);
    if (!q) return pairs;
    q.split('&').forEach(function (chunk) {
      if (!chunk) return;
      var eq = chunk.indexOf('=');
      if (eq < 0) pairs.push([chunk, '']);
      else pairs.push([chunk.slice(0, eq), chunk.slice(eq + 1)]);
    });
    return pairs;
  }

  function decodePlus(s) {
    try { return decodeURIComponent(String(s).replace(/\+/g, ' ')); }
    catch (e) { return String(s); }
  }

  function UrlSearchParams(init) {
    var pairs = [];
    if (typeof init === 'string') pairs = urlParamPairs(init);
    else if (Array.isArray(init)) {
      init.forEach(function (kv) { if (kv && kv.length === 2) pairs.push([String(kv[0]), String(kv[1])]); });
    } else if (init && typeof init === 'object') {
      Object.keys(init).forEach(function (k) { pairs.push([k, String(init[k])]); });
    }
    this.__pairs = pairs;
  }
  UrlSearchParams.prototype.append = function (k, v) { this.__pairs.push([String(k), String(v)]); };
  UrlSearchParams.prototype.get = function (k) {
    k = String(k);
    for (var i = 0; i < this.__pairs.length; ++i) if (this.__pairs[i][0] === k) return this.__pairs[i][1];
    return null;
  };
  UrlSearchParams.prototype.getAll = function (k) {
    k = String(k);
    return this.__pairs.filter(function (p) { return p[0] === k; }).map(function (p) { return p[1]; });
  };
  UrlSearchParams.prototype.has = function (k) { return this.get(k) !== null; };
  UrlSearchParams.prototype.set = function (k, v) {
    k = String(k); v = String(v);
    var done = false;
    this.__pairs = this.__pairs.filter(function (p) {
      if (p[0] !== k) return true;
      if (done) return false;
      done = true; p[1] = v; return true;
    });
    if (!done) this.__pairs.push([k, v]);
  };
  UrlSearchParams.prototype.delete = function (k) {
    k = String(k);
    this.__pairs = this.__pairs.filter(function (p) { return p[0] !== k; });
  };
  UrlSearchParams.prototype.sort = function () {
    this.__pairs.sort(function (a, b) { return a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0; });
  };
  UrlSearchParams.prototype.toString = function () {
    return this.__pairs.map(function (kv) {
      return encodeURIComponent(kv[0]).replace(/%20/g, '+') + '=' + encodeURIComponent(kv[1]).replace(/%20/g, '+');
    }).join('&');
  };
  UrlSearchParams.prototype.forEach = function (fn, thisArg) {
    var copy = this.__pairs.map(function (kv) { return kv.slice(); });
    copy.forEach(function (kv) { fn.call(thisArg, kv[1], kv[0], this); }, this);
  };
  UrlSearchParams.prototype.keys = function () {
    var out = []; var seen = {};
    this.__pairs.forEach(function (kv) { if (!seen[kv[0]]) { seen[kv[0]] = 1; out.push(kv[0]); } });
    return out;
  };
  UrlSearchParams.prototype.entries = function () { return this.__pairs.map(function (kv) { return kv.slice(); }); };
  try {
    UrlSearchParams.prototype[Symbol.iterator] = function () {
      var arr = this.entries(); var i = 0;
      return { next: function () { return i < arr.length ? { value: arr[i++], done: false } : { done: true }; } };
    };
  } catch (e) {}

  function URLPolyfill(href, base) {
    var resolved = parseUrl(href, base);
    var self = this;
    function sync() {
      self.protocol = resolved.protocol; self.origin = resolved.origin;
      self.host = resolved.host; self.hostname = resolved.hostname; self.port = resolved.port;
      self.pathname = resolved.pathname; self.search = resolved.search; self.hash = resolved.hash;
    }
    sync();
    this.username = ''; this.password = ''; this.searchParams = new UrlSearchParams(resolved.search);
    Object.defineProperty(this, 'href', {
      get: function () { return resolved.href; },
      set: function (v) { resolved = parseUrl(v); sync(); }
    });
  }
  URLPolyfill.prototype.toString = function () { return this.href; };
  URLPolyfill.createObjectURL = function () { return 'blob:prowsetk/' + Math.random().toString(36).slice(2); };
  URLPolyfill.revokeObjectURL = function () {};

  // ---------- event plumbing ----------
  function ListenerStore() { this.map = {}; }
  ListenerStore.prototype.add = function (type, fn, opts) {
    if (typeof fn !== 'function') return;
    var list = this.map[type] || (this.map[type] = []);
    for (var i = 0; i < list.length; ++i) {
      if (list[i].fn === fn && !!list[i].capture === !!(opts && opts.capture)) return;
    }
    list.push({ fn: fn, capture: !!(opts && opts.capture), once: !!(opts && (opts.once || opts === true)) });
  };
  ListenerStore.prototype.remove = function (type, fn) {
    var list = this.map[type];
    if (!list) return;
    this.map[type] = list.filter(function (e) { return e.fn !== fn; });
  };
  ListenerStore.prototype.take = function (type) {
    var list = this.map[type];
    if (!list) return [];
    if (list.length) this.map[type] = list.filter(function (e) { return !e.once; });
    return list.slice();
  };

  function makeEvent(type, init) {
    var ev = init && typeof init === 'object' ? init : {};
    var prevented = false;
    var stopped = false;
    var event = {
      type: String(type),
      bubbles: !!ev.bubbles,
      cancelable: !!ev.cancelable,
      detail: ev.detail,
      target: null,
      currentTarget: null,
      srcElement: null,
      defaultPrevented: false,
      isTrusted: false,
      eventPhase: 0,
      timeStamp: Date.now(),
      preventDefault: function () { prevented = true; event.defaultPrevented = true; },
      stopPropagation: function () { stopped = true; },
      stopImmediatePropagation: function () { stopped = true; },
      composedPath: function () { return []; }
    };
    if (typeof ev.relatedTarget !== 'undefined') event.relatedTarget = ev.relatedTarget;
    if (typeof ev.button !== 'undefined') event.button = ev.button;
    return event;
  }

  function fireStored(store, event, target) {
    event.target = target;
    var entries = store.take(event.type);
    for (var i = 0; i < entries.length; ++i) {
      try { entries[i].fn.call(target === undefined ? G : target, event); }
      catch (err) { reportError(err); }
    }
  }

  function reportError(err) {
    try {
      if (H && H.print) H.print('error', err && err.stack ? String(err.stack) : String(err));
      else if (console && console.error) console.error(err && err.stack ? String(err.stack) : String(err));
    } catch (e) {}
  }
  G.__prowsetkReportError = reportError;

  // ---------- element wrappers ----------
  var wrappers = new Map();
  var elementStores = new Map();

  function storeFor(handle) {
    var s = elementStores.get(handle);
    if (!s) { s = new ListenerStore(); elementStores.set(handle, s); }
    return s;
  }

  function wrap(handle) {
    if (!handle) return null;
    var existing = wrappers.get(handle);
    if (existing && H.nodeType(existing.__h) !== 0) return existing;
    if (existing) { wrappers.delete(handle); elementStores.delete(existing.__h); }
    var kind = H.nodeType(handle);
    var proto = kind === 3 ? TextNode.prototype
      : kind === 8 ? CommentNode.prototype
      : elementProtoFor(H.tagName(handle));
    var wrapper = Object.create(proto);
    wrapper.__h = handle;
    wrappers.set(handle, wrapper);
    return wrapper;
  }

  function ElementNode() {}
  function TextNode() {}
  function CommentNode() {}

  function attrString(el, name) { return H.attr(el.__h, name); }
  function setAttrString(el, name, value) {
    H.setAttr(el.__h, name, String(value));
    if (String(name).toLowerCase() === 'src') maybeExecuteScript(el);
  }

  function isConnectedHandle(handle) {
    var root = H.root();
    while (handle) {
      if (handle === root) return true;
      handle = H.parentNode(handle);
    }
    return false;
  }

  function fireElementCallback(el, type) {
    var event = makeEvent(type);
    fireStored(storeFor(el.__h), event, el);
    var handler = el['on' + type];
    if (typeof handler === 'function') {
      try { handler.call(el, event); } catch (err) { reportError(err); }
    }
  }

  function maybeExecuteScript(el) {
    if (!el || !el.__h || H.tagName(el.__h) !== 'script') return;
    if (el.__prowsetkScriptStarted || !isConnectedHandle(el.__h)) return;
    var source = H.attr(el.__h, 'src');
    if (source == null || source === '') return;
    if (/^(?:javascript|data|blob):/i.test(source)) return;
    el.__prowsetkScriptStarted = true;
    try {
      var resolved = H.resolveUrl(source, H.pageInfo().url || H.baseUrl()).href;
      var response = H.request('GET', resolved, [], undefined);
      if (!response.ok) {
        el.__prowsetkScriptError = response.error || ('script load failed: ' + resolved);
        fireElementCallback(el, 'error');
        return;
      }
      el.__prowsetkScriptLoaded = true;
      (0, eval)(String(response.body || '') + '\n//# sourceURL=' + resolved);
      fireElementCallback(el, 'load');
    } catch (err) {
      el.__prowsetkScriptError = err && err.message ? String(err.message) : String(err);
      reportError(err);
      fireElementCallback(el, 'error');
    }
  }

  Object.defineProperties(ElementNode.prototype, {
    nodeType: { get: function () { return H.nodeType(this.__h); }, enumerable: true },
    tagName: { get: function () { return H.tagName(this.__h).toUpperCase(); }, enumerable: true },
    localName: { get: function () { return H.tagName(this.__h); }, enumerable: true },
    nodeName: { get: function () { return H.tagName(this.__h).toUpperCase(); }, enumerable: true },
    id: {
      get: function () { return H.attr(this.__h, 'id') || ''; },
      set: function (v) { setAttrString(this, 'id', v); }, enumerable: true
    },
    className: {
      get: function () { return H.attr(this.__h, 'class') || ''; },
      set: function (v) { setAttrString(this, 'class', v); }, enumerable: true
    },
    title: {
      get: function () { return H.attr(this.__h, 'title') || ''; },
      set: function (v) { setAttrString(this, 'title', v); }
    },
    lang: {
      get: function () { return H.attr(this.__h, 'lang') || ''; },
      set: function (v) { setAttrString(this, 'lang', v); }
    },
    dir: {
      get: function () { return H.attr(this.__h, 'dir') || ''; },
      set: function (v) { setAttrString(this, 'dir', v); }
    },
    hidden: {
      get: function () { return H.hasAttr(this.__h, 'hidden'); },
      set: function (v) { if (v) setAttrString(this, 'hidden', ''); else H.delAttr(this.__h, 'hidden'); }
    },
    tabIndex: {
      get: function () { var v = H.attr(this.__h, 'tabindex'); return v == null ? -1 : parseInt(v, 10) || 0; },
      set: function (v) { setAttrString(this, 'tabindex', v); }
    },
    textContent: {
      get: function () { return H.text(this.__h); },
      set: function (v) {
        if (H.nodeType(this.__h) === 3 || H.nodeType(this.__h) === 8) H.setText(this.__h, String(v));
        else H.setText(this.__h, String(v));
      }, enumerable: true
    },
    innerText: {
      get: function () { return this.textContent; },
      set: function (v) { this.textContent = v; }
    },
    innerHTML: {
      get: function () { return H.innerHTML(this.__h); },
      set: function (v) { H.setInnerHTML(this.__h, String(v)); }, enumerable: true
    },
    outerHTML: {
      get: function () { return H.outerHTML(this.__h); },
      set: function (v) {
        var parent = H.parentNode(this.__h);
        if (!parent) return;
        var refs = H.parseFragment(String(v)).map(function (h) { return wrap(h); });
        var self = this;
        refs.forEach(function (node) { H.insertBefore(parent, node.__h, self.__h); });
        H.detach(self.__h);
      }
    },
    style: { get: function () { return styleFor(this); } },
    dataset: { get: function () { return datasetFor(this); } },
    classList: { get: function () { return classListFor(this); } },
    children: { get: function () {
      return H.childNodes(this.__h)
        .filter(function (h) { return H.nodeType(h) === 1; })
        .map(function (h) { return wrap(h); });
    } },
    childNodes: { get: function () { return H.childNodes(this.__h).map(function (h) { return wrap(h); }); } },
    firstChild: { get: function () { var kids = H.childNodes(this.__h); return kids.length ? wrap(kids[0]) : null; } },
    lastChild: { get: function () { var kids = H.childNodes(this.__h); return kids.length ? wrap(kids[kids.length - 1]) : null; } },
    firstElementChild: { get: function () { var kids = this.children; return kids.length ? kids[0] : null; } },
    lastElementChild: { get: function () { var kids = this.children; return kids.length ? kids[kids.length - 1] : null; } },
    parentNode: { get: function () { var p = H.parentNode(this.__h); return p ? wrap(p) : null; } },
    parentElement: { get: function () { var p = H.parentNode(this.__h); return p && H.nodeType(p) === 1 ? wrap(p) : null; } },
    nextSibling: { get: function () { var n = H.nextSibling(this.__h); return n ? wrap(n) : null; } },
    previousSibling: { get: function () { var p = H.prevSibling(this.__h); return p ? wrap(p) : null; } },
    nextElementSibling: { get: function () {
      var n = H.nextSibling(this.__h);
      while (n && H.nodeType(n) !== 1) n = H.nextSibling(n);
      return n ? wrap(n) : null;
    } },
    previousElementSibling: { get: function () {
      var p = H.prevSibling(this.__h);
      while (p && H.nodeType(p) !== 1) p = H.prevSibling(p);
      return p ? wrap(p) : null;
    } },
    ownerDocument: { get: function () { return document; } },
    attributes: { get: function () {
      return H.attrs(this.__h).map(function (kv) { return { name: kv[0], value: kv[1], nodeName: kv[0], nodeValue: kv[1] }; });
    } }
  });

  function defineValueProperty(proto, tagFilter) {
    Object.defineProperty(proto, 'value', {
      get: function () {
        var tag = H.tagName(this.__h);
        if (tag === 'textarea') return this.textContent;
        if (tag === 'select') {
          var sel = H.queryScope(this.__h, 'option[selected]')[0];
          if (!sel) sel = H.queryScope(this.__h, 'option')[0];
          if (!sel) return '';
          return H.attr(sel, 'value') != null ? H.attr(sel, 'value') : H.text(sel);
        }
        var v = H.attr(this.__h, 'value');
        return v == null ? this.textContent : v;
      },
      set: function (v) {
        var tag = H.tagName(this.__h);
        if (tag === 'textarea') { this.textContent = String(v); return; }
        if (tag === 'option') { if (H.attr(this.__h, 'value') == null) H.setText(this.__h, String(v)); }
        setAttrString(this, 'value', v);
      }, enumerable: true
    });
  }
  defineValueProperty(ElementNode.prototype);

  function defineCheckedProperty(proto) {
    Object.defineProperty(proto, 'checked', {
      get: function () { return H.hasAttr(this.__h, 'checked'); },
      set: function (v) { if (v) setAttrString(this, 'checked', 'checked'); else H.delAttr(this.__h, 'checked'); }
    });
    Object.defineProperty(proto, 'disabled', {
      get: function () { return H.hasAttr(this.__h, 'disabled'); },
      set: function (v) { if (v) setAttrString(this, 'disabled', 'disabled'); else H.delAttr(this.__h, 'disabled'); }
    });
    Object.defineProperty(proto, 'selected', {
      get: function () { return H.hasAttr(this.__h, 'selected'); },
      set: function (v) { if (v) setAttrString(this, 'selected', 'selected'); else H.delAttr(this.__h, 'selected'); }
    });
    Object.defineProperty(proto, 'readOnly', {
      get: function () { return H.hasAttr(this.__h, 'readonly'); },
      set: function (v) { if (v) setAttrString(this, 'readonly', 'readonly'); else H.delAttr(this.__h, 'readonly'); }
    });
  }
  defineCheckedProperty(ElementNode.prototype);

  function defineHrefProperty(proto) {
    ['href', 'src', 'action', 'poster'].forEach(function (name) {
      Object.defineProperty(proto, name, {
        get: function () {
          var raw = H.attr(this.__h, name);
          if (raw == null || raw === '') return raw == null ? '' : raw;
          if (raw.indexOf('javascript:') === 0 || raw.indexOf('data:') === 0 || raw.indexOf('blob:') === 0) return raw;
          return H.resolveUrl(raw, H.pageInfo().url || H.baseUrl()).href;
        },
        set: function (v) { setAttrString(this, name, v); }
      });
    });
  }
  defineHrefProperty(ElementNode.prototype);

  // HTMLAnchorElement URL decomposition attributes. Pages and libraries
  // (axios in particular) parse URLs by creating an <a> element, setting
  // href, and reading the components; the getters resolve against the live
  // document URL like defineHrefProperty above. Absent href yields the empty
  // string, matching the HTML standard's behavior for missing attributes.
  function defineUrlDecomposition(proto) {
    function partsFor(el) {
      var tag = H.tagName(el.__h);
      if (tag !== 'a' && tag !== 'area') return null;
      var raw = H.attr(el.__h, 'href');
      if (raw == null || raw === '') return null;
      if (raw.indexOf('javascript:') === 0 || raw.indexOf('data:') === 0 ||
          raw.indexOf('blob:') === 0 || raw.indexOf('#') === 0) return null;
      try {
        return H.resolveUrl(raw, H.pageInfo().url || H.baseUrl());
      } catch (e) {
        return null;
      }
    }
    function rebuiltHref(parts, name, value) {
      var protocol = String(parts.protocol || '').replace(/:$/, '');
      var hostname = String(parts.hostname || '');
      var port = String(parts.port || '');
      var pathname = String(parts.pathname || '/');
      var search = String(parts.search || '');
      var hash = String(parts.hash || '');
      if (name === 'protocol') {
        protocol = String(value).replace(/:$/, '');
      } else if (name === 'host') {
        var split = String(value).split(':');
        hostname = split[0];
        port = split.length > 1 ? split.slice(1).join(':') : '';
      } else if (name === 'hostname') {
        hostname = String(value);
      } else if (name === 'port') {
        port = String(value);
      } else if (name === 'pathname') {
        pathname = String(value).charAt(0) === '/' ? String(value) : '/' + value;
      } else if (name === 'search') {
        var s = String(value);
        search = s === '' || s.charAt(0) === '?' ? s : '?' + s;
      } else if (name === 'hash') {
        var h = String(value);
        hash = h === '' || h.charAt(0) === '#' ? h : '#' + h;
      }
      var host = hostname + (port ? ':' + port : '');
      return protocol + '://' + host + pathname + search + hash;
    }
    ['protocol', 'host', 'hostname', 'port', 'pathname', 'search', 'hash'].forEach(
      function (name) {
        Object.defineProperty(proto, name, {
          get: function () {
            var parts = partsFor(this);
            if (!parts) return '';
            return parts[name] == null ? '' : String(parts[name]);
          },
          set: function (v) {
            var parts = partsFor(this);
            if (!parts) return;
            setAttrString(this, 'href', rebuiltHref(parts, name, v));
          }, enumerable: true
        });
      });
    Object.defineProperty(proto, 'origin', {
      get: function () {
        var parts = partsFor(this);
        return parts && parts.origin ? String(parts.origin) : '';
      }, enumerable: true
    });
  }
  defineUrlDecomposition(ElementNode.prototype);

  ElementNode.prototype.getAttribute = function (name) {
    var v = H.attr(this.__h, String(name));
    return v == null ? null : v;
  };
  ElementNode.prototype.setAttribute = function (name, value) { setAttrString(this, name, value); };
  ElementNode.prototype.hasAttribute = function (name) { return H.hasAttr(this.__h, String(name)); };
  ElementNode.prototype.removeAttribute = function (name) { H.delAttr(this.__h, String(name)); };
  ElementNode.prototype.toggleAttribute = function (name, force) {
    var has = this.hasAttribute(name);
    var want = typeof force === 'undefined' ? !has : !!force;
    if (want) this.setAttribute(name, ''); else this.removeAttribute(name);
    return want;
  };
  ElementNode.prototype.matches = function (sel) { return H.matches(this.__h, String(sel)); };
  ElementNode.prototype.closest = function (sel) {
    sel = String(sel);
    var node = this;
    while (node && node.__h) {
      if (H.nodeType(node.__h) === 1 && H.matches(node.__h, sel)) return node;
      var p = H.parentNode(node.__h);
      node = p ? wrap(p) : null;
    }
    return null;
  };
  ElementNode.prototype.contains = function (other) {
    if (!other || !other.__h) return false;
    var node = other;
    while (node) {
      if (node.__h === this.__h) return true;
      var p = H.parentNode(node.__h);
      node = p ? wrap(p) : null;
    }
    return false;
  };
  ElementNode.prototype.querySelector = function (sel) {
    var hits = H.queryScope(this.__h, String(sel));
    return hits.length ? wrap(hits[0]) : null;
  };
  ElementNode.prototype.querySelectorAll = function (sel) {
    return H.queryScope(this.__h, String(sel)).map(function (h) { return wrap(h); });
  };
  ElementNode.prototype.getElementsByTagName = function (tag) {
    if (String(tag) === '*') return this.querySelectorAll('*');
    return H.queryScope(this.__h, String(tag).toLowerCase()).map(function (h) { return wrap(h); });
  };
  ElementNode.prototype.getElementsByClassName = function (cls) {
    return H.queryScope(this.__h, '.' + String(cls).trim().split(/\s+/).join('.'))
      .map(function (h) { return wrap(h); });
  };
  ElementNode.prototype.appendChild = function (child) {
    if (child && child.__h) {
      H.appendChild(this.__h, child.__h);
      maybeExecuteScript(child);
      return child;
    }
    if (child && child.__fragment) {
      child.__nodes.forEach(function (h) {
        H.appendChild(this.__h, h);
        maybeExecuteScript(wrap(h));
      }, this);
      child.__nodes = [];
      return child;
    }
    return child;
  };
  ElementNode.prototype.insertBefore = function (node, ref) {
    if (node && node.__h) {
      H.insertBefore(this.__h, node.__h, ref ? ref.__h : 0);
      maybeExecuteScript(node);
      return node;
    }
    return node;
  };
  ElementNode.prototype.removeChild = function (child) {
    if (child && child.__h) { H.detach(child.__h); }
    return child;
  };
  ElementNode.prototype.replaceChild = function (node, old) {
    this.insertBefore(node, old);
    if (old && old.__h) H.detach(old.__h);
    return old;
  };
  ElementNode.prototype.remove = function () { H.detach(this.__h); };
  ElementNode.prototype.cloneNode = function (deep) {
    var copy = document.createElement(H.tagName(this.__h));
    H.attrs(this.__h).forEach(function (kv) { copy.setAttribute(kv[0], kv[1]); });
    if (deep) copy.innerHTML = this.innerHTML;
    else copy.textContent = H.nodeType(this.__h) === 1 ? '' : this.textContent;
    if (H.nodeType(this.__h) !== 1) copy.textContent = this.textContent;
    return copy;
  };
  ElementNode.prototype.addEventListener = function (type, fn, opts) { storeFor(this.__h).add(String(type), fn, opts); };
  ElementNode.prototype.removeEventListener = function (type, fn) { storeFor(this.__h).remove(String(type), fn); };
  ElementNode.prototype.dispatch = function (type, init) {
    var event = makeEvent(type, init);
    fireStored(storeFor(this.__h), event, this);
    if (event.bubbles) {
      var p = H.parentNode(this.__h);
      while (p && !event.__stopped) { fireStored(storeFor(p), event, wrap(p)); p = H.parentNode(p); }
    }
    return !event.defaultPrevented;
  };
  ElementNode.prototype.dispatchEvent = function (event) {
    if (!event || !event.type) return true;
    fireStored(storeFor(this.__h), event, this);
    if (event.bubbles) {
      var p = H.parentNode(this.__h);
      while (p) { fireStored(storeFor(p), event, wrap(p)); p = H.parentNode(p); }
    }
    return !event.defaultPrevented;
  };
  ElementNode.prototype.focus = function () { document.activeElement = this; };
  ElementNode.prototype.blur = function () { if (document.activeElement === this) document.activeElement = document.body; };
  ElementNode.prototype.click = function () {
    var handled = this.dispatch('click', { bubbles: true, cancelable: true });
    if (H.tagName(this.__h) === 'a' && handled) {
      var href = this.href;
      if (href && href.indexOf('javascript:') !== 0) navigateTo(href);
    }
    if (H.tagName(this.__h) === 'button' && handled) {
      var form = this.closest ? this.closest('form') : null;
      if (form) form.submit();
    }
  };
  ElementNode.prototype.getBoundingClientRect = function () {
    return { x: 0, y: 0, top: 0, left: 0, right: 0, bottom: 0, width: 0, height: 0 };
  };
  ElementNode.prototype.scrollIntoView = function () {};
  ElementNode.prototype.attachShadow = function () { return document.createElement('shadow-root'); };
  ElementNode.prototype.getBoundingClientRect = function () {
    return { x: 0, y: 0, top: 0, left: 0, right: 0, bottom: 0, width: 0, height: 0 };
  };
  ElementNode.prototype.requestSubmit = function () {
    var form = this.closest ? this.closest('form') : null;
    if (form) form.submit();
  };

  function formFields(form) {
    var fields = [];
    H.queryScope(form.__h, 'input, textarea, select').map(function (h) { return wrap(h); }).forEach(function (el) {
      var name = el.getAttribute('name');
      if (!name) return;
      var type = (el.getAttribute('type') || '').toLowerCase();
      if (type === 'submit' || type === 'button' || type === 'file' || type === 'reset') return;
      if ((type === 'checkbox' || type === 'radio') && !el.checked) return;
      var value = el.value;
      fields.push([name, value == null ? '' : String(value)]);
    });
    return fields;
  }

  ElementNode.prototype.submit = function () {
    if (H.tagName(this.__h) !== 'form') return;
    var method = (this.getAttribute('method') || 'GET').toUpperCase();
    var action = this.action || H.pageInfo().url;
    var fields = formFields(this);
    var query = fields.map(function (kv) {
      return encodeURIComponent(kv[0]) + '=' + encodeURIComponent(kv[1]);
    }).join('&');
    if (method === 'POST') {
      H.submitForm(action, 'POST', query);
      return;
    }
    var target = action;
    if (query) target += (action.indexOf('?') >= 0 ? '&' : '?') + query;
    navigateTo(target, false);
  };
  ElementNode.prototype.reset = function () {
    if (H.tagName(this.__h) !== 'form') return;
    formFields(this).length;  // no-op reset: Flatworm does not snapshot initial values
  };

  TextNode.prototype = Object.create(ElementNode.prototype);
  Object.defineProperty(TextNode.prototype, 'nodeType',
                       { get: function () { return 3; } });
  CommentNode.prototype = Object.create(ElementNode.prototype);
  Object.defineProperty(CommentNode.prototype, 'nodeType',
                       { get: function () { return 8; } });
  Object.defineProperty(TextNode.prototype, 'textContent', {
    get: function () { return H.text(this.__h); },
    set: function (v) { H.setText(this.__h, String(v)); }
  });
  Object.defineProperty(CommentNode.prototype, 'data', {
    get: function () { return H.text(this.__h); },
    set: function (v) { H.setText(this.__h, String(v)); }
  });

  // ---------- DOM interface constructors ----------
  // Pages and diagnostics libraries run `instanceof` checks against the
  // standard DOM interfaces (error trackers walk window.HTMLIFrameElement,
  // frameworks check HTMLInputElement, ...). The wrappers above stay a single
  // implementation class; these interfaces graft the standard prototype
  // chain onto it so instanceof resolves the way pages expect:
  //   instance -> HTMLAnchorElement.prototype -> HTMLElement.prototype ->
  //   ElementNode.prototype (the implementation) -> Element.prototype ->
  //   Node.prototype.
  // Like a browser, the interface constructors themselves are not
  // constructible.
  function interfaceCtor(name, parentProto) {
    var ctor = function () { throw new TypeError('Illegal constructor'); };
    try {
      Object.defineProperty(ctor, 'name', { value: name, configurable: true });
    } catch (e) {}
    ctor.prototype = Object.create(parentProto);
    Object.defineProperty(ctor.prototype, 'constructor',
                          { value: ctor, writable: true, configurable: true });
    return ctor;
  }
  var DOMInterfaces = {};
  DOMInterfaces.Node = interfaceCtor('Node', Object.prototype);
  DOMInterfaces.CharacterData =
    interfaceCtor('CharacterData', DOMInterfaces.Node.prototype);
  DOMInterfaces.Element = interfaceCtor('Element', DOMInterfaces.Node.prototype);
  DOMInterfaces.HTMLElement =
    interfaceCtor('HTMLElement', DOMInterfaces.Element.prototype);
  DOMInterfaces.SVGElement =
    interfaceCtor('SVGElement', DOMInterfaces.Element.prototype);
  DOMInterfaces.HTMLMediaElement =
    interfaceCtor('HTMLMediaElement', DOMInterfaces.HTMLElement.prototype);
  DOMInterfaces.HTMLAudioElement =
    interfaceCtor('HTMLAudioElement', DOMInterfaces.HTMLMediaElement.prototype);
  DOMInterfaces.HTMLVideoElement =
    interfaceCtor('HTMLVideoElement', DOMInterfaces.HTMLMediaElement.prototype);
  ['HTMLAnchorElement', 'HTMLAreaElement', 'HTMLBRElement', 'HTMLBaseElement',
   'HTMLBodyElement', 'HTMLButtonElement', 'HTMLCanvasElement', 'HTMLDListElement',
   'HTMLDataListElement', 'HTMLDetailsElement', 'HTMLDialogElement',
   'HTMLDirectoryElement', 'HTMLDivElement', 'HTMLEmbedElement',
   'HTMLFieldSetElement', 'HTMLFontElement', 'HTMLFormElement',
   'HTMLFrameElement', 'HTMLFrameSetElement', 'HTMLHRElement',
   'HTMLHeadElement', 'HTMLHeadingElement', 'HTMLHtmlElement',
   'HTMLIFrameElement', 'HTMLImageElement', 'HTMLInputElement', 'HTMLLIElement',
   'HTMLLabelElement', 'HTMLLegendElement', 'HTMLLinkElement', 'HTMLMapElement',
   'HTMLMarqueeElement', 'HTMLMenuElement', 'HTMLMetaElement', 'HTMLMeterElement',
   'HTMLModElement', 'HTMLOListElement', 'HTMLObjectElement',
   'HTMLOptGroupElement', 'HTMLOptionElement', 'HTMLOutputElement',
   'HTMLParagraphElement', 'HTMLParamElement', 'HTMLPictureElement',
   'HTMLPreElement', 'HTMLProgressElement', 'HTMLQuoteElement',
   'HTMLScriptElement', 'HTMLSelectElement', 'HTMLSlotElement',
   'HTMLSourceElement', 'HTMLSpanElement', 'HTMLStyleElement',
   'HTMLTableCaptionElement', 'HTMLTableCellElement', 'HTMLTableColElement',
   'HTMLTableElement', 'HTMLTableRowElement', 'HTMLTableSectionElement',
   'HTMLTemplateElement', 'HTMLTextAreaElement', 'HTMLTimeElement',
   'HTMLTitleElement', 'HTMLTrackElement', 'HTMLUListElement',
   'HTMLUnknownElement'].forEach(function (name) {
    DOMInterfaces[name] =
      interfaceCtor(name, DOMInterfaces.HTMLElement.prototype);
  });
  var TAG_INTERFACES = {
    a: 'HTMLAnchorElement', area: 'HTMLAreaElement', audio: 'HTMLAudioElement',
    base: 'HTMLBaseElement', blockquote: 'HTMLQuoteElement',
    body: 'HTMLBodyElement', br: 'HTMLBRElement', button: 'HTMLButtonElement',
    canvas: 'HTMLCanvasElement', caption: 'HTMLTableCaptionElement',
    cite: 'HTMLQuoteElement', col: 'HTMLTableColElement',
    colgroup: 'HTMLTableColElement', datalist: 'HTMLDataListElement',
    del: 'HTMLModElement', details: 'HTMLDetailsElement',
    dialog: 'HTMLDialogElement', dir: 'HTMLDirectoryElement',
    div: 'HTMLDivElement', dl: 'HTMLDListElement', embed: 'HTMLEmbedElement',
    fieldset: 'HTMLFieldSetElement', font: 'HTMLFontElement',
    form: 'HTMLFormElement', frame: 'HTMLFrameElement',
    frameset: 'HTMLFrameSetElement', h1: 'HTMLHeadingElement',
    h2: 'HTMLHeadingElement', h3: 'HTMLHeadingElement', h4: 'HTMLHeadingElement',
    h5: 'HTMLHeadingElement', h6: 'HTMLHeadingElement', head: 'HTMLHeadElement',
    hr: 'HTMLHRElement', html: 'HTMLHtmlElement', iframe: 'HTMLIFrameElement',
    img: 'HTMLImageElement', input: 'HTMLInputElement', ins: 'HTMLModElement',
    label: 'HTMLLabelElement', legend: 'HTMLLegendElement', li: 'HTMLLIElement',
    link: 'HTMLLinkElement', map: 'HTMLMapElement', marquee: 'HTMLMarqueeElement',
    menu: 'HTMLMenuElement', meta: 'HTMLMetaElement', meter: 'HTMLMeterElement',
    object: 'HTMLObjectElement', ol: 'HTMLOListElement',
    optgroup: 'HTMLOptGroupElement', option: 'HTMLOptionElement',
    output: 'HTMLOutputElement', p: 'HTMLParagraphElement',
    param: 'HTMLParamElement', picture: 'HTMLPictureElement',
    pre: 'HTMLPreElement', progress: 'HTMLProgressElement',
    q: 'HTMLQuoteElement', script: 'HTMLScriptElement', select: 'HTMLSelectElement',
    slot: 'HTMLSlotElement', source: 'HTMLSourceElement', span: 'HTMLSpanElement',
    style: 'HTMLStyleElement', svg: 'SVGElement', table: 'HTMLTableElement',
    tbody: 'HTMLTableSectionElement', td: 'HTMLTableCellElement',
    template: 'HTMLTemplateElement', textarea: 'HTMLTextAreaElement',
    tfoot: 'HTMLTableSectionElement', th: 'HTMLTableCellElement',
    thead: 'HTMLTableSectionElement', time: 'HTMLTimeElement',
    title: 'HTMLTitleElement', tr: 'HTMLTableRowElement',
    track: 'HTMLTrackElement', ul: 'HTMLUListElement', video: 'HTMLVideoElement'
  };
  function elementProtoFor(tag) {
    var name = TAG_INTERFACES[tag];
    var ctor = name ? DOMInterfaces[name] : null;
    return ctor ? ctor.prototype : DOMInterfaces.HTMLUnknownElement.prototype;
  }
  Object.setPrototypeOf(ElementNode.prototype, DOMInterfaces.Element.prototype);
  Object.setPrototypeOf(DOMInterfaces.HTMLElement.prototype, ElementNode.prototype);
  Object.setPrototypeOf(DOMInterfaces.SVGElement.prototype, ElementNode.prototype);
  Object.setPrototypeOf(TextNode.prototype, DOMInterfaces.CharacterData.prototype);
  Object.setPrototypeOf(CommentNode.prototype, DOMInterfaces.CharacterData.prototype);

  // ---------- style / class / dataset views ----------
  function parseStyle(cssText) {
    var out = {};
    String(cssText || '').split(';').forEach(function (rule) {
      var idx = rule.indexOf(':');
      if (idx > 0) {
        var k = rule.slice(0, idx).trim().toLowerCase();
        var v = rule.slice(idx + 1).trim();
        if (k) out[k] = v;
      }
    });
    return out;
  }

  function styleFor(element) {
    function currentMap() { return parseStyle(H.attr(element.__h, 'style') || ''); }
    function writeMap(map) {
      var text = Object.keys(map).map(function (k) { return k + ': ' + map[k]; }).join('; ');
      if (text) setAttrString(element, 'style', text);
      else H.delAttr(element.__h, 'style');
    }
    function kebab(prop) { return prop.replace(/[A-Z]/g, function (c) { return '-' + c.toLowerCase(); }).toLowerCase(); }
    function camel(name) { return name.replace(/-([a-z])/g, function (m, c) { return c.toUpperCase(); }); }
    var proxy = new Proxy({
      getPropertyValue: function (name) {
        var v = currentMap()[kebab(name)];
        return v == null ? '' : v;
      },
      setProperty: function (name, value) {
        var map = currentMap(); map[kebab(name)] = String(value); writeMap(map);
      },
      removeProperty: function (name) {
        var map = currentMap(); var key = kebab(name);
        var old = map[key]; delete map[key]; writeMap(map);
        return old == null ? '' : old;
      },
      item: function (i) { return Object.keys(currentMap())[i] || ''; }
    }, {
      get: function (target, prop) {
        if (typeof prop === 'symbol') return undefined;
        if (prop === 'cssText') {
          var map = currentMap();
          return Object.keys(map).map(function (k) { return k + ': ' + map[k]; }).join('; ');
        }
        if (prop in target) return target[prop];
        var v = currentMap()[kebab(prop)];
        return v == null ? '' : v;
      },
      set: function (target, prop, value) {
        if (prop === 'cssText') {
          writeMap(parseStyle(value));
          return true;
        }
        var map = currentMap();
        var key = kebab(prop);
        if (value === '' || value == null) delete map[key];
        else map[key] = String(value);
        writeMap(map);
        return true;
      },
      has: function (target, prop) { return true; },
      ownKeys: function () { return Object.keys(currentMap()).map(camel); },
      getOwnPropertyDescriptor: function (target, prop) {
        var map = currentMap();
        var key = kebab(prop);
        if (map[key] != null) return { value: map[key], writable: true, enumerable: true, configurable: true };
        return undefined;
      }
    });
    return proxy;
  }

  function classListFor(element) {
    function tokens() {
      var v = H.attr(element.__h, 'class') || '';
      return v.split(/\s+/).filter(function (t) { return t.length > 0; });
    }
    function write(list) {
      if (list.length) setAttrString(element, 'class', list.join(' '));
      else H.delAttr(element.__h, 'class');
    }
    var list = {
      item: function (i) { return tokens()[i] || null; },
      contains: function (name) { return tokens().indexOf(String(name)) >= 0; },
      add: function () {
        var cur = tokens();
        Array.prototype.forEach.call(arguments, function (name) {
          if (cur.indexOf(String(name)) < 0) cur.push(String(name));
        });
        write(cur);
      },
      remove: function () {
        var cur = tokens();
        Array.prototype.forEach.call(arguments, function (name) {
          var idx = cur.indexOf(String(name));
          if (idx >= 0) cur.splice(idx, 1);
        });
        write(cur);
      },
      toggle: function (name, force) {
        var cur = tokens();
        var idx = cur.indexOf(String(name));
        var want = typeof force === 'undefined' ? idx < 0 : !!force;
        if (want && idx < 0) cur.push(String(name));
        if (!want && idx >= 0) cur.splice(idx, 1);
        write(cur);
        return want;
      },
      replace: function (from, to) {
        var cur = tokens();
        var idx = cur.indexOf(String(from));
        if (idx < 0) return false;
        cur[idx] = String(to);
        write(cur);
        return true;
      },
      toString: function () { return tokens().join(' '); },
      value: { get: function () { return tokens().join(' '); }, set: function (v) { write(String(v).split(/\s+/)); } }
    };
    Object.defineProperty(list, 'length', { get: function () { return tokens().length; } });
    return list;
  }

  function datasetFor(element) {
    function read(prop) {
      var attr = 'data-' + prop.replace(/[A-Z]/g, function (c) { return '-' + c.toLowerCase(); });
      var v = H.attr(element.__h, attr);
      return v == null ? undefined : v;
    }
    function write(prop, value) {
      var attr = 'data-' + prop.replace(/[A-Z]/g, function (c) { return '-' + c.toLowerCase(); });
      if (value === undefined || value === null) H.delAttr(element.__h, attr);
      else setAttrString(element, attr, value);
    }
    return new Proxy({
      getOwnPropertyDescriptor: function (target, prop) {
        if (prop === 'length') return { value: 0, writable: false };
        return undefined;
      }
    }, {
      get: function (target, prop) {
        if (typeof prop === 'symbol') return undefined;
        if (prop === 'toJSON') return function () {
          var out = {};
          H.attrs(element.__h).forEach(function (kv) {
            if (kv[0].indexOf('data-') === 0) out[camel(kv[0].slice(5))] = kv[1];
          });
          return out;
        };
        return read(prop);
      },
      set: function (target, prop, value) { write(prop, value); return true; },
      has: function (target, prop) { return read(prop) !== undefined; },
      ownKeys: function () {
        return H.attrs(element.__h)
          .filter(function (kv) { return kv[0].indexOf('data-') === 0; })
          .map(function (kv) { return camel(kv[0].slice(5)); });
      },
      getOwnPropertyDescriptor: function (target, prop) {
        var v = read(prop);
        return v === undefined ? undefined : { value: v, enumerable: true, configurable: true };
      }
    });
    function camel(name) { return name.replace(/-([a-z])/g, function (m, c) { return c.toUpperCase(); }); }
  }

  // ---------- document ----------
  var documentTarget = new ListenerStore();
  var activeElementHandle = { value: null };

  var document = {
    nodeType: 9,
    documentElement: null,
    get readyState() { return docState.readyState; },
    set readyState(v) { docState.readyState = String(v); },
    get visibilityState() { return 'visible'; },
    get hidden() { return false; },
    get title() { return H.pageInfo().title; },
    set title(v) { H.setTitle(String(v)); },
    get URL() { return H.pageInfo().url; },
    get documentURI() { return H.pageInfo().url; },
    get baseURI() { return H.baseUrl(); },
    get referrer() { return H.pageInfo().referrer; },
    get cookie() { return H.getCookie(); },
    set cookie(v) { H.setCookie(String(v)); },
    get characterSet() { return 'UTF-8'; },
    get charset() { return 'UTF-8'; },
    get inputEncoding() { return 'UTF-8'; },
    compatMode: 'CSS1Compat',
    get contentType() { return 'text/html'; },
    get doctype() { return { name: 'html', publicId: '', systemId: '' }; },
    get implementation() {
      return {
        createHTMLDocument: function () { return document; },
        hasFeature: function () { return true; }
      };
    },
    get activeElement() { return activeElementHandle.value || document.body || document.documentElement; },
    set activeElement(v) { activeElementHandle.value = v; },
    get documentElement() {
      var root = H.root();
      return root ? wrap(root) : null;
    },
    get head() {
      var hits = H.queryAll('head');
      return hits.length ? wrap(hits[0]) : document.documentElement;
    },
    get body() {
      var hits = H.queryAll('body');
      if (hits.length) return wrap(hits[0]);
      var html = H.root();
      if (!html) return null;
      var body = H.createEl('body');
      H.appendChild(html, body);
      return wrap(body);
    },
    set body(node) {
      var current = H.queryAll('body')[0];
      if (current && node && node.__h) { H.appendChild(H.parentNode(current), node.__h); }
    },
    get forms() { return H.queryAll('form').map(function (h) { return wrap(h); }); },
    get links() { return H.queryAll('a[href]').map(function (h) { return wrap(h); }); },
    get images() { return H.queryAll('img').map(function (h) { return wrap(h); }); },
    get scripts() { return H.queryAll('script').map(function (h) { return wrap(h); }); },
    get styleSheets() { return []; },
    get all() { return H.queryAll('*').map(function (h) { return wrap(h); }); },
    querySelector: function (sel) {
      var hits = H.queryAll(String(sel));
      return hits.length ? wrap(hits[0]) : null;
    },
    querySelectorAll: function (sel) {
      return H.queryAll(String(sel)).map(function (h) { return wrap(h); });
    },
    getElementById: function (id) {
      id = String(id);
      try {
        var hits = H.queryAll('#' + id);
        if (hits.length) return wrap(hits[0]);
      } catch (e) { /* fall through to scan */ }
      var all = H.queryAll('*');
      for (var i = 0; i < all.length; ++i) {
        if (H.attr(all[i], 'id') === id) return wrap(all[i]);
      }
      return null;
    },
    getElementsByTagName: function (tag) {
      tag = String(tag);
      if (tag === '*') return H.queryAll('*').map(function (h) { return wrap(h); });
      return H.queryAll(tag.toLowerCase()).map(function (h) { return wrap(h); });
    },
    getElementsByName: function (name) {
      return H.queryAll('[name]').map(function (h) { return wrap(h); })
        .filter(function (el) { return el.getAttribute('name') === String(name); });
    },
    getElementsByClassName: function (cls) {
      var names = String(cls).trim().split(/\s+/).filter(Boolean);
      if (!names.length) return [];
      return H.queryAll('*').map(function (h) { return wrap(h); })
        .filter(function (el) {
          return names.every(function (name) { return (el.className || '').split(/\s+/).indexOf(name) >= 0; });
        });
    },
    createElement: function (tag) { return wrap(H.createEl(String(tag))); },
    createElementNS: function (ns, tag) { return document.createElement(tag); },
    createTextNode: function (text) { return wrap(H.createText(String(text))); },
    createComment: function (text) { return wrap(H.createComment(String(text))); },
    createDocumentFragment: function () {
      var frag = { nodeType: 11, __fragment: true, __nodes: [], childNodes: [], appendChild: function (child) {
        if (child && child.__h) { this.__nodes.push(child.__h); this.childNodes.push(child); }
        return child;
      } };
      return frag;
    },
    createEvent: function (kind) { return { initEvent: function (type, bubbles, cancelable) {
      var self = this;
      self.type = type; self.bubbles = !!bubbles; self.cancelable = !!cancelable;
    }, type: '', bubbles: false, cancelable: false, preventDefault: function () {}, stopPropagation: function () {} }; },
    createExpression: function () { return { evaluate: function () { return { stringResultValue: '', numberValue: 0, booleanValue: false }; } }; },
    write: function (markup) {
      var handles = H.parseFragment(String(markup));
      var html = H.root();
      if (!html) return;
      var target = H.queryAll('body')[0] || html;
      handles.forEach(function (h) { H.appendChild(target, h); });
    },
    writeln: function (markup) { document.write(String(markup) + '\n'); },
    open: function () {},
    close: function () {},
    addEventListener: function (type, fn, opts) { documentTarget.add(String(type), fn, opts); },
    removeEventListener: function (type, fn) { documentTarget.remove(String(type), fn); },
    dispatchEvent: function (event) {
      if (!event || !event.type) return true;
      fireStored(documentTarget, event, document);
      return !event.defaultPrevented;
    },
    elementFromPoint: function () { return null; },
    getSelection: function () { return { toString: function () { return ''; }, removeAllRanges: function () {} }; },
    createRange: function () { return { selectNodeContents: function () {}, toString: function () { return ''; } }; },
    createTreeWalker: function () { return { nextNode: function () { return null; } }; },
    createNodeIterator: function () { return { nextNode: function () { return null; } }; }
  };
  document.addEventListener = function (type, fn, opts) {
    if (typeof fn === 'function' || (fn && typeof fn.handleEvent === 'function')) {
      documentTarget.add(String(type), typeof fn === 'function' ? fn : function (ev) { fn.handleEvent(ev); }, opts);
    }
  };
  document.dispatchEvent = function (event) {
    if (!event || !event.type) return true;
    fireStored(documentTarget, event, document);
    return !event.defaultPrevented;
  };
  document.getElementById = function (id) {
    id = String(id);
    var hits = H.queryAll('#' + id.replace(/[^A-Za-z0-9_-]/g, function (c) { return '\\' + c; }));
    for (var i = 0; i < hits.length; ++i) {
      if (H.attr(hits[i], 'id') === id) return wrap(hits[i]);
    }
    var all = H.queryAll('*');
    for (var j = 0; j < all.length; ++j) {
      if (H.attr(all[j], 'id') === id) return wrap(all[j]);
    }
    return null;
  };

  // ---------- location ----------
  var locationState = { override: null };
  function currentHref() {
    if (locationState.override) return locationState.override;
    return H.pageInfo().url;
  }
  function locationParts() {
    return parseUrl(currentHref(), null);
  }
  var location = {
    replace: function (url) { navigateTo(url, true); },
    assign: function (url) { navigateTo(url, false); },
    reload: function () { navigateTo(currentHref(), true); }
  };
  ['href', 'origin', 'protocol', 'host', 'hostname', 'port', 'pathname', 'search', 'hash'].forEach(function (key) {
    Object.defineProperty(location, key, {
      get: function () { return locationParts()[key] || ''; },
      set: function (v) {
        if (key === 'href') { navigateTo(v, false); return; }
        var target = locationParts();
        var base = (target.protocol || '') + '//' + (target.host || '');
        var next = base + (key === 'pathname' ? v : (target.pathname || '/')) +
          (key === 'search' ? (String(v).charAt(0) === '?' ? v : '?' + v) : (target.search || '')) +
          (key === 'hash' ? (String(v).charAt(0) === '#' ? v : '#' + v) : (target.hash || ''));
        navigateTo(next, false);
      }
    });
  });
  location.toString = function () { return location.href; };
  document.location = location;
  document.defaultView = G;

  function navigateTo(url, replace) {
    var resolved = parseUrl(url, null);
    if (!resolved || !resolved.href) return;
    if (resolved.href.indexOf('javascript:') === 0) return;
    locationState.override = resolved.href;
    H.navigate(resolved.href);
  }

  // ---------- window event target + timers + lifecycle ----------
  var windowTarget = new ListenerStore();
  var timers = [];
  var nextTimerId = 1;
  var rafCbs = [];

  function setTimeoutJs(fn, delay) {
    var args = Array.prototype.slice.call(arguments, 2);
    var id = nextTimerId++;
    var timer = { id: id, when: Date.now() + (Number(delay) || 0), fn: fn,
                  args: args, interval: 0, period: 0, active: true };
    timers.push(timer);
    return id;
  }
  function setIntervalJs(fn, delay) {
    var args = Array.prototype.slice.call(arguments, 2);
    var id = nextTimerId++;
    var period = Math.max(0, Number(delay) || 0);
    var timer = { id: id, when: Date.now() + period, fn: fn, args: args,
                  interval: 1, period: period, active: true };
    timers.push(timer);
    return id;
  }
  var cancelledTimers = new Set();
  function cancelTimer(id) {
    cancelledTimers.add(id);
    for (var i = 0; i < timers.length; ++i) {
      if (timers[i].id === id) { timers[i].active = false; }
    }
    timers = timers.filter(function (t) { return !cancelledTimers.has(t.id); });
    for (var j = 0; j < rafCbs.length; ++j) {
      if (rafCbs[j].id === id) rafCbs[j].fn = null;
    }
  }
  function clearTimeoutJs(id) { cancelTimer(id); }
  function requestAnimationFrame(fn) {
    var id = nextTimerId++;
    rafCbs.push({ id: id, fn: fn });
    return id;
  }
  function cancelAnimationFrame(id) {
    for (var j = 0; j < rafCbs.length; ++j) {
      if (rafCbs[j].id === id) rafCbs[j].fn = null;
    }
  }

  var lifecycleQueue = ['DOMContentLoaded', 'load'];
  var MAX_FLUSH_CALLBACKS = 500;

  function fireLifecycle(name) {
    if (docState.fired[name]) return 0;
    docState.fired[name] = true;
    if (name === 'DOMContentLoaded') {
      docState.readyState = 'interactive';
      var event = makeEvent('DOMContentLoaded');
      fireStored(documentTarget, event, document);
      fireStored(windowTarget, event, G);
      if (typeof G.onreadystatechange === 'function') { try { G.onreadystatechange(); } catch (e) { reportError(e); } }
      return 1;
    }
    if (name === 'load') {
      docState.readyState = 'complete';
      var loadEvent = makeEvent('load');
      fireStored(windowTarget, loadEvent, G);
      if (typeof G.onload === 'function') { try { G.onload(loadEvent); } catch (e) { reportError(e); } }
      var docLoad = makeEvent('load');
      fireStored(documentTarget, docLoad, document);
      return 1;
    }
    return 0;
  }

  function __prowsetkFlush() {
    var fired = 0;
    while (fired < MAX_FLUSH_CALLBACKS) {
      var ran = 0;
      if (lifecycleQueue.length) { ran += fireLifecycle(lifecycleQueue.shift()); }
      if (ran) { fired += ran; continue; }
      var now = Date.now();
      var due = null;
      for (var i = 0; i < timers.length; ++i) {
        if (timers[i].when <= now && (!due || due.when > timers[i].when)) due = timers[i];
      }
      if (rafCbs.length && !due) due = { when: now, fn: function () { var snapshot = rafCbs; rafCbs = []; snapshot.forEach(function (r) { try { r.fn(now); } catch (e) { reportError(e); } }); }, args: [], interval: 0, id: 0 };
      if (!due) break;
      timers = timers.filter(function (t) { return t.id !== due.id; });
      var fn = due.fn;
      if (fn) {
        try {
          if (typeof fn === 'string') (0, eval)(fn);
          else fn.apply(G, due.args);
        } catch (e) { reportError(e); }
      }
      if (due.interval && due.active && !cancelledTimers.has(due.id)) {
        cancelledTimers.delete(due.id);
        due.when = Date.now() + due.period;
        timers.push(due);
      }
      fired += 1;
      var microtasks = H.drainJobs();
    }
    return fired;
  }

  // ---------- XMLHttpRequest ----------
  function XMLHttpRequest() {
    this.readyState = 0;
    this.status = 0;
    this.statusText = '';
    this.response = '';
    this.responseText = '';
    this.responseXML = null;
    this.responseType = '';
    this.timeout = 0;
    this.withCredentials = false;
    this.upload = { addEventListener: function () {}, removeEventListener: function () {} };
    this.__method = 'GET';
    this.__url = '';
    this.__headers = [];
    this.__async = true;
    this.__open = false;
    this.__sent = false;
    this.__finalUrl = '';
    this.__responseHeaders = [];
  }
  XMLHttpRequest.prototype.open = function (method, url, async) {
    this.__method = String(method || 'GET').toUpperCase();
    this.__url = String(url || '');
    this.__async = async !== false;
    this.__open = true;
    this.readyState = 1;
    this.__fire('readystatechange');
  };
  XMLHttpRequest.prototype.setRequestHeader = function (name, value) {
    this.__headers.push([String(name), String(value)]);
  };
  XMLHttpRequest.prototype.getResponseHeader = function (name) {
    name = String(name).toLowerCase();
    var found = this.__responseHeaders.filter(function (kv) { return kv[0].toLowerCase() === name; })[0];
    return found ? found[1] : null;
  };
  XMLHttpRequest.prototype.getAllResponseHeaders = function () {
    return this.__responseHeaders.map(function (kv) { return kv[0] + ': ' + kv[1]; }).join('\r\n');
  };
  XMLHttpRequest.prototype.overrideMimeType = function (mime) { this.__mimeType = mime; };
  XMLHttpRequest.prototype.abort = function () {
    this.__aborted = true;
    if (this.__sent) {
      this.readyState = 0; this.status = 0;
      this.__fire('abort');
      this.__fire('loadend');
    }
  };
  XMLHttpRequest.prototype.addEventListener = function (type, fn, opts) {
    var store = this.__listeners || (this.__listeners = {});
    (store[String(type)] || (store[String(type)] = [])).push(fn);
  };
  XMLHttpRequest.prototype.removeEventListener = function (type, fn) {
    var store = this.__listeners || (this.__listeners = {});
    var key = String(type);
    if (store[key]) store[key] = store[key].filter(function (f) { return f !== fn; });
  };
  XMLHttpRequest.prototype.dispatchEvent = function (ev) { this.__fire(ev.type, ev); return true; };
  XMLHttpRequest.prototype.__fire = function (type, event) {
    var ev = event || makeEvent(type);
    ev.target = this;
    var store = this.__listeners || {};
    (store[type] || []).forEach(function (fn) { try { fn.call(this, ev); } catch (e) { reportError(e); } }, this);
    var handler = this['on' + type];
    if (typeof handler === 'function') { try { handler.call(this, ev); } catch (e) { reportError(e); } }
  };
  XMLHttpRequest.prototype.send = function (body) {
    var self = this;
    if (!this.__open || this.__sent) return;
    this.__sent = true;
    var run = function () {
      var response = H.request(self.__method, H.resolveUrl(self.__url, H.pageInfo().url || H.baseUrl()).href, self.__headers, body == null ? undefined : String(body));
      self.__responseHeaders = response.headers || [];
      if (!response.ok) {
        self.readyState = 4;
        self.status = 0;
        self.__fire('readystatechange');
        self.__fire('error');
        self.__fire('loadend');
        return;
      }
      self.status = response.status;
      self.statusText = response.statusText || '';
      self.responseText = response.body || '';
      if (self.responseType === 'json') {
        try { self.response = JSON.parse(self.responseText); }
        catch (e) { self.response = null; }
      } else {
        self.response = self.responseText;
      }
      self.__finalUrl = response.finalUrl || '';
      self.readyState = 2;
      self.__fire('readystatechange');
      self.readyState = 3;
      self.__fire('readystatechange');
      self.readyState = 4;
      self.__fire('readystatechange');
      self.__fire('load');
      self.__fire('loadend');
    };
    if (this.__async) queueMicrotask(run);
    else run();
  };

  // ---------- fetch ----------
  function Headers(init) {
    this.__entries = [];
    var self = this;
    if (init && typeof init.forEach === 'function' && typeof init !== 'string') {
      try { init.forEach(function (v, k) { self.__entries.push([String(k), String(v)]); }); } catch (e) {}
    } else if (Array.isArray(init)) {
      init.forEach(function (kv) { if (kv && kv.length >= 2) self.__entries.push([String(kv[0]), String(kv[1])]); });
    } else if (init && typeof init === 'object') {
      Object.keys(init).forEach(function (k) { self.__entries.push([k, String(init[k])]); });
    }
  }
  Headers.prototype.get = function (name) {
    name = String(name).toLowerCase();
    var found = this.__entries.filter(function (kv) { return kv[0].toLowerCase() === name; })[0];
    return found ? found[1] : null;
  };
  Headers.prototype.set = function (name, value) {
    name = String(name).toLowerCase();
    this.__entries = this.__entries.filter(function (kv) { return kv[0].toLowerCase() !== name; });
    this.__entries.push([name, String(value)]);
  };
  Headers.prototype.append = function (name, value) { this.__entries.push([String(name), String(value)]); };
  Headers.prototype.has = function (name) { return this.get(name) !== null; };
  Headers.prototype.delete = function (name) {
    name = String(name).toLowerCase();
    this.__entries = this.__entries.filter(function (kv) { return kv[0].toLowerCase() !== name; });
  };
  Headers.prototype.forEach = function (fn, thisArg) {
    var copy = this.__entries.map(function (kv) { return kv.slice(); });
    copy.forEach(function (kv) { fn.call(thisArg, kv[1], kv[0], this); }, this);
  };
  Headers.prototype.entries = function () { return this.__entries.map(function (kv) { return kv.slice(); }); };

  function ResponseImpl(response, init) {
    // Host form: the shim's fetch passes the record returned by H.request,
    // which always carries finalUrl. Anything else is treated as the
    // web-standard `new Response(body, init)` construction, including the
    // argumentless `new Response` probe axios (and other libraries) use for
    // feature detection.
    if (response == null || typeof response !== 'object' ||
        !('finalUrl' in response)) {
      var record = {
        status: init && init.status != null ? init.status : 200,
        statusText: init && init.statusText ? init.statusText : '',
        finalUrl: init && init.url ? init.url : '',
        headers: [],
        body: response == null ? '' : String(response)
      };
      var raw = init && init.headers;
      if (raw != null) {
        if (raw instanceof Headers) {
          record.headers = raw.__entries.map(function (kv) { return kv.slice(); });
        } else if (Array.isArray(raw)) {
          raw.forEach(function (kv) {
            if (kv && kv.length >= 2) {
              record.headers.push([String(kv[0]), String(kv[1])]);
            }
          });
        } else if (typeof raw.forEach === 'function') {
          raw.forEach(function (v, k) { record.headers.push([String(k), String(v)]); });
        } else if (typeof raw === 'object') {
          Object.keys(raw).forEach(function (k) {
            record.headers.push([String(k), String(raw[k])]);
          });
        }
      }
      response = record;
    }
    this.ok = response.status >= 200 && response.status < 300;
    this.status = response.status || 0;
    this.statusText = response.statusText || '';
    this.url = response.finalUrl || '';
    this.redirected = false;
    this.type = 'basic';
    this.headers = new Headers();
    (response.headers || []).forEach(function (kv) { this.headers.append(kv[0], kv[1]); }, this);
    this.__body = response.body || '';
    this.bodyUsed = false;
  }
  ResponseImpl.prototype.text = function () {
    this.bodyUsed = true;
    return Promise.resolve(this.__body);
  };
  ResponseImpl.prototype.json = function () {
    var self = this;
    return this.text().then(function (text) { return JSON.parse(text); });
  };
  ResponseImpl.prototype.arrayBuffer = function () {
    return Promise.resolve(H.toBytes(this.__body));
  };
  ResponseImpl.prototype.blob = function () { return Promise.resolve({ size: (this.__body || '').length, type: '' }); };

  function fetch(input, init) {
    init = init || {};
    var url = typeof input === 'string' ? input : (input && input.url) || String(input);
    var method = String(init.method || (input && input.method) || 'GET').toUpperCase();
    var headers = [];
    var rawHeaders = init.headers;
    if (rawHeaders) {
      if (Array.isArray(rawHeaders)) {
        rawHeaders.forEach(function (kv) { headers.push([String(kv[0]), String(kv[1])]); });
      } else if (typeof rawHeaders.forEach === 'function') {
        rawHeaders.forEach(function (v, k) { headers.push([String(k), String(v)]); });
      } else {
        Object.keys(rawHeaders).forEach(function (k) { headers.push([k, String(rawHeaders[k])]); });
      }
    }
    var body = init.body;
    if (body != null && typeof body !== 'string' && body.toString && !(body instanceof ArrayBuffer)) body = String(body);
    var resolved = H.resolveUrl(url, H.pageInfo().url || H.baseUrl());
    return new Promise(function (resolve, reject) {
      queueMicrotask(function () {
        var response;
        try {
          response = H.request(method, resolved.href, headers, body == null ? undefined : String(body));
        } catch (e) { reject(new TypeError('Failed to fetch')); return; }
        if (!response.ok) { reject(new TypeError(response.error || 'Failed to fetch')); return; }
        resolve(new ResponseImpl(response));
      });
    });
  }
  fetch.polyfill = true;

  // ---------- storage ----------
  function makeStorage(area) {
    return {
      get length() { return H.storageKeys(area).length; },
      key: function (i) { var keys = H.storageKeys(area); return i < keys.length ? keys[i] : null; },
      getItem: function (k) {
        var value = H.storageGet(area, String(k));
        return typeof value === 'undefined' ? null : value;
      },
      setItem: function (k, v) { H.storageSet(area, String(k), String(v)); },
      removeItem: function (k) { H.storageRemove(area, String(k)); },
      clear: function () { H.storageClear(area); }
    };
  }

  // ---------- misc ----------
  function EventCtor(type, init) { return makeEvent(type, init); }
  function CustomEventCtor(type, init) { return makeEvent(type, init); }

  function DOMParserStub() {}
  DOMParserStub.prototype.parseFromString = function (markup, mime) {
    var handles = H.parseFragment(String(markup));
    var fake = Object.create(document);
    var rootHandle = H.createEl('html');
    var bodyHandle = H.createEl('body');
    handles.forEach(function (h) { H.appendChild(bodyHandle, h); });
    H.appendChild(rootHandle, bodyHandle);
    Object.defineProperty(fake, 'documentElement',
                          { value: wrap(rootHandle), configurable: true });
    Object.defineProperty(fake, 'body',
                          { value: wrap(bodyHandle), configurable: true });
    fake.querySelector = function (sel) { var hits = H.queryScope(rootHandle, String(sel)); return hits.length ? wrap(hits[0]) : null; };
    fake.querySelectorAll = function (sel) { return H.queryScope(rootHandle, String(sel)).map(function (h) { return wrap(h); }); };
    fake.getElementsByTagName = function (tag) { return fake.querySelectorAll(String(tag) === '*' ? '*' : String(tag).toLowerCase()); };
    return fake;
  };

  function FormDataStub(form) {
    this.__entries = [];
    if (form && form.__h) {
      var self = this;
      H.queryScope(form.__h, 'input, textarea, select').map(function (h) { return wrap(h); }).forEach(function (el) {
        var name = el.getAttribute('name');
        if (!name) return;
        var type = (el.getAttribute('type') || '').toLowerCase();
        if (type === 'submit' || type === 'button' || type === 'file') return;
        if ((type === 'checkbox' || type === 'radio') && !el.checked) return;
        self.__entries.push([name, el.value == null ? '' : String(el.value)]);
      });
    }
  }
  FormDataStub.prototype.append = function (k, v) { this.__entries.push([String(k), String(v)]); };
  FormDataStub.prototype.get = function (k) {
    var found = this.__entries.filter(function (kv) { return kv[0] === String(k); })[0];
    return found ? found[1] : null;
  };
  FormDataStub.prototype.getAll = function (k) {
    return this.__entries.filter(function (kv) { return kv[0] === String(k); }).map(function (kv) { return kv[1]; });
  };
  FormDataStub.prototype.has = function (k) { return this.get(k) !== null; };
  FormDataStub.prototype.set = function (k, v) {
    k = String(k);
    this.__entries = this.__entries.filter(function (kv) { return kv[0] !== k; });
    this.__entries.push([k, String(v)]);
  };
  FormDataStub.prototype.delete = function (k) {
    k = String(k);
    this.__entries = this.__entries.filter(function (kv) { return kv[0] !== k; });
  };
  FormDataStub.prototype.entries = function () { return this.__entries.map(function (kv) { return kv.slice(); }); };
  FormDataStub.prototype.forEach = function (fn) { this.__entries.forEach(function (kv) { fn(kv[1], kv[0], this); }, this); };
  FormDataStub.prototype.toString = function () {
    return this.__entries.map(function (kv) {
      return encodeURIComponent(kv[0]) + '=' + encodeURIComponent(kv[1]);
    }).join('&');
  };
  try {
    FormDataStub.prototype[Symbol.iterator] = function () {
      var arr = this.entries(); var i = 0;
      return { next: function () { return i < arr.length ? { value: arr[i++], done: false } : { done: true }; } };
    };
  } catch (e) {}

  function ObserverStub(behavior) {
    return function ObserverCtor(callback) {
      this.callback = callback;
      this.__observed = [];
    };
    ObserverCtor.prototype.observe = function (target, opts) { this.__observed.push([target, opts]); };
    ObserverCtor.prototype.unobserve = function () {};
    ObserverCtor.prototype.disconnect = function () { this.__observed = []; };
    ObserverCtor.prototype.takeRecords = function () { return []; };
    return ObserverCtor;
  }
  var MutationObserverCtor = (function () {
    function MutationObserver(callback) { this.callback = callback; }
    MutationObserver.prototype.observe = function () {};
    MutationObserver.prototype.unobserve = function () {};
    MutationObserver.prototype.disconnect = function () {};
    MutationObserver.prototype.takeRecords = function () { return []; };
    return MutationObserver;
  })();

  function ImageCtor(width, height) {
    var img = document.createElement('img');
    if (width) img.setAttribute('width', width);
    if (height) img.setAttribute('height', height);
    var source = '';
    Object.defineProperty(img, 'src', {
      get: function () { return source; },
      set: function (v) {
        source = String(v);
        img.setAttribute('src', source);
        queueMicrotask(function () {
          var response = H.request('GET', source, [], undefined);
          if (response.ok) img.dispatch('load'); else img.dispatch('error');
        });
      }
    });
    return img;
  }

  function WorkerStub(url) {
    this.url = url;
    this.onmessage = null;
  }
  WorkerStub.prototype.postMessage = function () {};
  WorkerStub.prototype.terminate = function () {};
  WorkerStub.prototype.addEventListener = function () {};
  WorkerStub.prototype.removeEventListener = function () {};

  function pluginArray(entries) {
    entries = entries || [];
    var array = entries.slice();
    array.item = function (index) { return index >= 0 && index < array.length ? array[index] : null; };
    array.namedItem = function (name) {
      for (var i = 0; i < array.length; ++i) {
        if (array[i] && (array[i].name === name || array[i].type === name)) return array[i];
      }
      return null;
    };
    array.refresh = function () {};
    return array;
  }

  // ---------- install globals ----------
  function install(name, value, force) {
    if (!force && typeof G[name] !== 'undefined') return;
    G[name] = value;
  }
  install('document', document, true);
  install('XMLHttpRequest', XMLHttpRequest, true);
  install('fetch', fetch, true);
  install('Headers', Headers, true);
  install('Response', ResponseImpl, true);
  Object.keys(DOMInterfaces).forEach(function (name) {
    install(name, DOMInterfaces[name], true);
  });
  install('Request', function RequestCtor(url, opts) { this.url = url; this.method = (opts && opts.method) || 'GET'; }, true);
  install('URL', URLPolyfill, true);
  install('URLSearchParams', UrlSearchParams, true);
  install('Event', EventCtor, true);
  install('CustomEvent', CustomEventCtor, true);
  install('DOMParser', DOMParserStub, true);
  install('FormData', FormDataStub, true);
  install('MutationObserver', MutationObserverCtor, true);
  install('IntersectionObserver', ObserverStub(), true);
  install('ResizeObserver', ObserverStub(), true);
  install('PerformanceObserver', ObserverStub(), true);
  install('Image', ImageCtor, true);
  install('Worker', WorkerStub, true);
  install('SharedWorker', WorkerStub, true);
  install('localStorage', makeStorage('local'), true);
  install('sessionStorage', makeStorage('session'), true);
  install('setTimeout', setTimeoutJs, true);
  install('setInterval', setIntervalJs, true);
  install('clearTimeout', clearTimeoutJs, true);
  install('clearInterval', clearTimeoutJs, true);
  install('requestAnimationFrame', requestAnimationFrame, true);
  install('cancelAnimationFrame', cancelAnimationFrame, true);
  install('webkitRequestAnimationFrame', requestAnimationFrame, true);
  install('__prowsetkFlush', __prowsetkFlush, true);

  var nav = H.navigatorInfo();
  var ua = String(nav.userAgent || '');
  var chromeLike = /Chrome\//.test(ua) || /Chromium\//.test(ua);
  var mimeTypes = pluginArray([{ type: 'application/pdf', suffixes: 'pdf', description: 'Portable Document Format' }]);
  var plugins = pluginArray([{ name: 'Chrome PDF Viewer', filename: 'internal-pdf-viewer', description: 'Portable Document Format', 0: mimeTypes[0], length: 1 }]);
  var navigator = {
    userAgent: nav.userAgent,
    appVersion: nav.userAgent.replace(/^.*Mozilla\//, ''),
    platform: nav.platform,
    vendor: chromeLike ? 'Google Inc.' : '',
    vendorSub: '',
    language: nav.language,
    languages: [nav.language],
    onLine: nav.onLine,
    cookieEnabled: nav.cookieEnabled,
    hardwareConcurrency: 4,
    maxTouchPoints: 0,
    product: 'Gecko',
    productSub: '20030107',
    appName: 'Netscape',
    appCodeName: 'Mozilla',
    doNotTrack: null,
    webdriver: false,
    plugins: plugins,
    mimeTypes: mimeTypes,
    pdfViewerEnabled: true,
    javaEnabled: function () { return false; },
    sendBeacon: function (url, data) {
      H.request('POST', H.resolveUrl(String(url), H.pageInfo().url || H.baseUrl()).href, [], data == null ? undefined : String(data));
      return true;
    },
    getBattery: function () { return Promise.resolve({ charging: true, level: 1 }); },
    permissions: { query: function () { return Promise.resolve({ state: 'prompt' }); } },
    mediaDevices: { getUserMedia: function () { return Promise.reject(new Error('not supported')); } },
    connection: { effectiveType: '4g', downlink: 10, rtt: 50 }
  };
  if (chromeLike) {
    navigator.userAgentData = {
      brands: [
        { brand: 'Chromium', version: '128' },
        { brand: 'Google Chrome', version: '128' }
      ],
      mobile: false,
      platform: 'Linux',
      getHighEntropyValues: function (hints) {
        var out = { mobile: false, platform: 'Linux' };
        (hints || []).forEach(function (hint) {
          if (hint === 'architecture') out.architecture = 'x86';
          else if (hint === 'bitness') out.bitness = '64';
          else if (hint === 'platformVersion') out.platformVersion = '0.0.0';
          else if (hint === 'uaFullVersion') out.uaFullVersion = '128.0.0.0';
          else if (hint === 'fullVersionList') out.fullVersionList = [
            { brand: 'Chromium', version: '128.0.0.0' },
            { brand: 'Google Chrome', version: '128.0.0.0' }
          ];
        });
        return Promise.resolve(out);
      }
    };
  }
  install('navigator', navigator, true);
  install('window', G, true);
  install('self', G, true);
  install('top', G, true);
  install('parent', G, true);
  install('frames', G, true);
  if (chromeLike) {
    install('chrome', {
      runtime: {},
      app: { isInstalled: false },
      csi: function () { return {}; },
      loadTimes: function () { return {}; }
    }, true);
  }

  install('location', location, true);
  install('history', {
    length: 1, state: null, scrollRestoration: 'auto',
    pushState: function (state, title, url) { if (url != null) navigateTo(url, false); },
    replaceState: function (state, title, url) { if (url != null) navigateTo(url, true); },
    back: function () {}, forward: function () {}, go: function () {}
  }, true);
  install('screen', { width: 1920, height: 1080, availWidth: 1920, availHeight: 1040, colorDepth: 24, pixelDepth: 24, orientation: { type: 'landscape-primary', angle: 0, lock: function () {}, unlock: function () {} } }, true);
  install('devicePixelRatio', 1, true);
  install('innerWidth', 1280, true);
  install('innerHeight', 800, true);
  install('outerWidth', 1280, true);
  install('outerHeight', 800, true);
  install('scrollX', 0, true);
  install('scrollY', 0, true);
  install('pageXOffset', 0, true);
  install('pageYOffset', 0, true);
  install('visualViewport', { width: 1280, height: 800, offsetLeft: 0, offsetTop: 0, scale: 1 }, true);
  install('performance', {
    now: function () { return Date.now(); },
    timeOrigin: Date.now(),
    timing: { navigationStart: Date.now(), loadEventStart: 0, loadEventEnd: 0 },
    memory: { usedJSHeapSize: 16000000, totalJSHeapSize: 16000000 },
    getEntriesByType: function () { return []; },
    mark: function () {}, measure: function () {}, clearMarks: function () {}, clearMeasures: function () {}
  }, true);

  G.getComputedStyle = function (el) {
    var map = {};
    if (el && el.__h) map = parseStyle(H.attr(el.__h, 'style') || '');
    map.getPropertyValue = function (name) {
      name = String(name).toLowerCase();
      var v = map[name];
      return v == null ? '' : v;
    };
    return map;
  };
  G.matchMedia = function (query) {
    return {
      matches: false, media: String(query),
      addListener: function () {}, removeListener: function () {},
      addEventListener: function () {}, removeEventListener: function () {},
      dispatchEvent: function () { return false; },
      onchange: null
    };
  };
  G.alert = function (message) { if (H.print) H.print('alert', String(message == null ? '' : message)); };
  G.confirm = function () { return false; };
  G.prompt = function () { return null; };
  G.open = function () { return null; };
  G.close = function () {};
  G.scrollTo = function () {};
  G.scrollBy = function () {};
  G.scroll = function () {};
  G.print = function () {};
  G.stop = function () {};
  G.focus = function () {};
  G.blur = function () {};
  G.addEventListener = function (type, fn, opts) { windowTarget.add(String(type), fn, opts); };
  G.removeEventListener = function (type, fn) { windowTarget.remove(String(type), fn); };
  G.dispatchEvent = function (event) {
    if (!event || !event.type) return true;
    fireStored(windowTarget, event, G);
    var handler = G['on' + event.type];
    if (typeof handler === 'function') { try { handler.call(G, event); } catch (e) { reportError(e); } }
    return !event.defaultPrevented;
  };
  var B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=';
  G.atob = function (input) {
    var s = String(input).replace(/[\s]/g, '');
    var output = '';
    var bits = 0, value = 0;
    for (var i = 0; i < s.length; ++i) {
      var idx = B64.indexOf(s.charAt(i));
      if (idx < 0 || idx === 64) break;
      value = (value << 6) | idx;
      bits += 6;
      if (bits >= 8) {
        bits -= 8;
        output += String.fromCharCode((value >> bits) & 0xFF);
      }
    }
    return output;
  };
  G.btoa = function (input) {
    var s = String(input);
    var output = '';
    for (var i = 0; i < s.length; i += 3) {
      var a = s.charCodeAt(i);
      var b = i + 1 < s.length ? s.charCodeAt(i + 1) : -1;
      var c = i + 2 < s.length ? s.charCodeAt(i + 2) : -1;
      output += B64.charAt(a >> 2);
      output += B64.charAt(((a & 3) << 4) | (b < 0 ? 0 : b >> 4));
      output += b < 0 ? '=' : B64.charAt(((b & 15) << 2) | (c < 0 ? 0 : c >> 6));
      output += c < 0 ? '=' : B64.charAt(c & 63);
    }
    return output;
  };

  // ---------- Encoding Standard: TextEncoder / TextDecoder ----------
  // UTF-8 (the standard's only required encoding) plus the common
  // windows-1252/latin-1 labels as a courtesy. Unsupported labels throw
  // RangeError, invalid input throws TypeError when fatal, and is otherwise
  // replaced with U+FFFD. Streaming mode retains a bounded partial-sequence
  // prefix across decode() calls.
  function utf8EncodeCodePoint(out, code) {
    if (code < 0x80) out.push(code);
    else if (code < 0x800) out.push(0xC0 | (code >> 6), 0x80 | (code & 0x3F));
    else if (code < 0x10000) {
      out.push(0xE0 | (code >> 12), 0x80 | ((code >> 6) & 0x3F), 0x80 | (code & 0x3F));
    } else {
      out.push(0xF0 | (code >> 18), 0x80 | ((code >> 12) & 0x3F),
               0x80 | ((code >> 6) & 0x3F), 0x80 | (code & 0x3F));
    }
  }
  function stringCodePointAt(s, i) {
    var code = s.charCodeAt(i);
    if (code >= 0xD800 && code <= 0xDBFF && i + 1 < s.length) {
      var next = s.charCodeAt(i + 1);
      if (next >= 0xDC00 && next <= 0xDFFF) {
        return [0x10000 + ((code - 0xD800) << 10) + (next - 0xDC00), i + 2];
      }
    }
    return [code, i + 1];
  }
  G.TextEncoder = function TextEncoder() {
    this.encoding = 'utf-8';
  };
  G.TextEncoder.prototype.encode = function (input) {
    var s = String(input == null ? '' : input);
    var out = [];
    var i = 0;
    while (i < s.length) {
      var cp = stringCodePointAt(s, i);
      var code = cp[0];
      i = cp[1];
      if (code >= 0xD800 && code <= 0xDFFF) code = 0xFFFD;
      utf8EncodeCodePoint(out, code);
    }
    var bytes = new Uint8Array(out.length);
    for (var j = 0; j < out.length; ++j) bytes[j] = out[j];
    return bytes;
  };
  G.TextEncoder.prototype.encodeInto = function (source, destination) {
    var s = String(source == null ? '' : source);
    if (!destination || typeof destination !== 'object' ||
        typeof destination.length !== 'number') {
      throw new TypeError('TextEncoder.encodeInto requires a typed array');
    }
    var limit = destination.length;
    var written = 0;
    var read = 0;
    var i = 0;
    while (i < s.length) {
      var cp = stringCodePointAt(s, i);
      var code = cp[0];
      i = cp[1];
      if (code >= 0xD800 && code <= 0xDFFF) code = 0xFFFD;
      var tmp = [];
      utf8EncodeCodePoint(tmp, code);
      if (written + tmp.length > limit) break;
      for (var j = 0; j < tmp.length; ++j) destination[written + j] = tmp[j];
      written += tmp.length;
      read = i;
    }
    return { read: read, written: written };
  };
  var W1252 = [0x20AC, 0x81, 0x201A, 0x192, 0x201E, 0x2026, 0x2020, 0x2021,
               0x2C6, 0x2030, 0x160, 0x2039, 0x152, 0x8D, 0x17D, 0x8F,
               0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
               0x2DC, 0x2122, 0x161, 0x203A, 0x153, 0x9D, 0x17E, 0x178];
  function encodingFromLabel(label) {
    var s = String(label == null ? 'utf-8' : label)
      .toLowerCase().replace(/[^a-z0-9]/g, '');
    if (s === '' || s === 'utf8' || s === 'unicode11utf8' ||
        s === 'unicode20utf8' || s === 'xunicode20utf8') {
      return 'utf-8';
    }
    if (s === 'latin1' || s === 'iso88591' || s === 'windows1252' ||
        s === 'cp1252' || s === 'xcp1252' || s === 'ascii' ||
        s === 'usascii' || s === 'ansix341968') {
      return 'windows-1252';
    }
    return null;
  }
  function utf8DecodeChunk(bytes, start, end, fatal) {
    var out = '';
    var i = start;
    while (i < end) {
      var b = bytes[i];
      if (b < 0x80) { out += String.fromCharCode(b); i += 1; continue; }
      var need, code;
      if (b >= 0xC2 && b <= 0xDF) { need = 1; code = b & 0x1F; }
      else if (b >= 0xE0 && b <= 0xEF) { need = 2; code = b & 0x0F; }
      else if (b >= 0xF0 && b <= 0xF4) { need = 3; code = b & 0x07; }
      else {
        if (fatal) throw new TypeError('invalid UTF-8 sequence');
        out += '\uFFFD';
        i += 1;
        continue;
      }
      if (i + need >= end) break;
      var valid = true;
      for (var j = 1; j <= need; ++j) {
        var cont = bytes[i + j];
        if (cont < 0x80 || cont > 0xBF) { valid = false; break; }
        code = (code << 6) | (cont & 0x3F);
      }
      if (valid) {
        if ((b === 0xE0 && bytes[i + 1] < 0xA0) ||
            (b === 0xF0 && bytes[i + 1] < 0x90) ||
            (b === 0xF4 && bytes[i + 1] > 0x8F) ||
            (b === 0xED && bytes[i + 1] >= 0xA0)) {
          valid = false;
        }
      }
      if (!valid) {
        if (fatal) throw new TypeError('invalid UTF-8 sequence');
        out += '\uFFFD';
        i += 1;
        continue;
      }
      if (code < 0x10000) {
        out += String.fromCharCode(code);
      } else {
        code -= 0x10000;
        out += String.fromCharCode(0xD800 | (code >> 10), 0xDC00 | (code & 0x3FF));
      }
      i += need + 1;
    }
    return [out, i];
  }
  G.TextDecoder = function TextDecoder(label, options) {
    var encoding = encodingFromLabel(label);
    if (encoding === null) {
      throw new RangeError('unsupported encoding label');
    }
    options = options || {};
    this.encoding = encoding;
    this.fatal = !!options.fatal;
    this.ignoreBOM = !!options.ignoreBOM;
    this._pending = [];
    this._bomChecked = false;
  };
  G.TextDecoder.prototype.decode = function (input, options) {
    options = options || {};
    var stream = !!options.stream;
    var bytes = [];
    if (input !== undefined && input !== null) {
      var view = null;
      if (input instanceof ArrayBuffer) {
        view = new Uint8Array(input);
      } else if (typeof input === 'object' && input !== null &&
                 input.buffer instanceof ArrayBuffer &&
                 typeof input.byteOffset === 'number' &&
                 typeof input.byteLength === 'number') {
        view = new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
      } else {
        throw new TypeError('TextDecoder.decode requires a buffer source');
      }
      for (var k = 0; k < view.length; ++k) bytes.push(view[k]);
    }
    if (this._pending.length) {
      bytes = this._pending.concat(bytes);
      this._pending = [];
    }
    if (!this._bomChecked && !this.ignoreBOM && this.encoding === 'utf-8' &&
        bytes.length >= 3 && bytes[0] === 0xEF && bytes[1] === 0xBB && bytes[2] === 0xBF) {
      bytes = bytes.slice(3);
    }
    this._bomChecked = true;
    var out = '';
    if (this.encoding === 'windows-1252') {
      for (var m = 0; m < bytes.length; ++m) {
        var b = bytes[m];
        if (b < 0x80 || b > 0x9F) out += String.fromCharCode(b);
        else out += String.fromCharCode(W1252[b - 0x80]);
      }
    } else {
      var decoded = utf8DecodeChunk(bytes, 0, bytes.length, this.fatal);
      out = decoded[0];
      var consumed = decoded[1];
      if (consumed < bytes.length) {
        if (stream) {
          this._pending = bytes.slice(consumed);
        } else if (!this.fatal) {
          for (var r = consumed; r < bytes.length; ++r) out += '\uFFFD';
        } else {
          throw new TypeError('invalid UTF-8 sequence');
        }
      }
      if (!stream) this._bomChecked = false;
    }
    return out;
  };

  windowTarget.add('error', function () {});
  G.onerror = null;
  G.onload = null;
  G.onunload = null;
  G.onbeforeunload = null;
  G.onhashchange = null;
  G.onpopstate = null;
  G.onresize = null;
  G.onscroll = null;
  G.onorientationchange = null;
  document.onload = null;
  document.onreadystatechange = null;
})();
)SHIM";

}  // namespace prowsetk

#endif  // PROWSETK_WEB_PLATFORM_SHIM_HPP
