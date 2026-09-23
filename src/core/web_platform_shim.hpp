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

  // ---------- DOM events ----------
  // Capture, target, and bubble follow the DOM dispatch algorithm. There is
  // no layout hit-testing and no trusted user input: events are synthetic.
  var CAPTURING_PHASE = 1, AT_TARGET = 2, BUBBLING_PHASE = 3;

  function listenerOptions(opts) {
    if (opts === true) return { capture: true, once: false, passive: false, signal: null };
    if (opts == null || opts === false) return { capture: false, once: false, passive: false, signal: null };
    return {
      capture: !!opts.capture,
      once: !!opts.once,
      passive: !!opts.passive,
      signal: opts.signal || null
    };
  }

  function ListenerStore() { this.list = []; }
  ListenerStore.prototype.add = function (type, callback, opts) {
    var options = listenerOptions(opts);
    var fn = callback;
    if (typeof callback !== 'function') {
      if (!callback || typeof callback.handleEvent !== 'function') return;
      fn = function (event) { callback.handleEvent(event); };
    }
    type = String(type);
    for (var i = 0; i < this.list.length; ++i) {
      var existing = this.list[i];
      if (!existing.removed && existing.type === type && existing.callback === callback &&
          existing.capture === options.capture) return;
    }
    var entry = {
      type: type, fn: fn, callback: callback, capture: options.capture,
      once: options.once, passive: options.passive, removed: false
    };
    if (options.signal && options.signal.aborted) return;
    this.list.push(entry);
    if (options.signal && typeof options.signal.addEventListener === 'function') {
      var store = this;
      options.signal.addEventListener('abort', function () {
        store.remove(type, callback, options.capture);
      }, { once: true });
    }
  };
  ListenerStore.prototype.remove = function (type, callback, capture) {
    var wantCapture = capture === true || !!(capture && typeof capture === 'object' && capture.capture);
    type = String(type);
    for (var i = 0; i < this.list.length; ++i) {
      var entry = this.list[i];
      if (entry.type === type && entry.callback === callback && entry.capture === wantCapture) {
        entry.removed = true;
      }
    }
    this.list = this.list.filter(function (entry) { return !entry.removed; });
  };

  function Event(type, init) {
    if (!(this instanceof Event)) throw new TypeError("Failed to construct 'Event'");
    init = init || {};
    this.type = String(type);
    this.bubbles = !!init.bubbles;
    this.cancelable = !!init.cancelable;
    this.composed = !!init.composed;
    this.detail = init.detail === undefined ? null : init.detail;
    this.target = null;
    this.currentTarget = null;
    this.srcElement = null;
    this.relatedTarget = init.relatedTarget || null;
    this.eventPhase = 0;
    this.timeStamp = Date.now();
    this.defaultPrevented = false;
    this.returnValue = true;
    this.cancelBubble = false;
    this.isTrusted = false;
    this.__stop = false;
    this.__stopImmediate = false;
    this.__passive = false;
    this.__path = [];
  }
  Event.prototype.preventDefault = function () {
    if (this.__passive || !this.cancelable) return;
    this.defaultPrevented = true;
    this.returnValue = false;
  };
  Event.prototype.stopPropagation = function () { this.__stop = true; this.cancelBubble = true; };
  Event.prototype.stopImmediatePropagation = function () {
    this.__stop = true;
    this.__stopImmediate = true;
    this.cancelBubble = true;
  };
  Event.prototype.composedPath = function () { return this.__path.slice(); };
  Event.prototype.initEvent = function (type, bubbles, cancelable) {
    this.type = String(type);
    this.bubbles = !!bubbles;
    this.cancelable = !!cancelable;
  };
  Event.NONE = 0;
  Event.CAPTURING_PHASE = CAPTURING_PHASE;
  Event.AT_TARGET = AT_TARGET;
  Event.BUBBLING_PHASE = BUBBLING_PHASE;

  function UIEvent(type, init) {
    if (!(this instanceof UIEvent)) throw new TypeError("Failed to construct 'UIEvent'");
    init = init || {};
    Event.call(this, type, init);
    this.detail = init.detail == null ? 0 : init.detail;
    this.view = init.view || G;
  }
  UIEvent.prototype = Object.create(Event.prototype);
  UIEvent.prototype.constructor = UIEvent;

  function numOrZero(value) { return typeof value === 'number' && isFinite(value) ? value : 0; }

  function MouseEvent(type, init) {
    if (!(this instanceof MouseEvent)) throw new TypeError("Failed to construct 'MouseEvent'");
    init = init || {};
    UIEvent.call(this, type, init);
    this.screenX = numOrZero(init.screenX);
    this.screenY = numOrZero(init.screenY);
    this.clientX = numOrZero(init.clientX);
    this.clientY = numOrZero(init.clientY);
    this.pageX = numOrZero(init.pageX);
    this.pageY = numOrZero(init.pageY);
    this.button = numOrZero(init.button);
    this.buttons = numOrZero(init.buttons);
    this.ctrlKey = !!init.ctrlKey;
    this.shiftKey = !!init.shiftKey;
    this.altKey = !!init.altKey;
    this.metaKey = !!init.metaKey;
    this.relatedTarget = init.relatedTarget || null;
  }
  MouseEvent.prototype = Object.create(UIEvent.prototype);
  MouseEvent.prototype.constructor = MouseEvent;

  function PointerEvent(type, init) {
    if (!(this instanceof PointerEvent)) throw new TypeError("Failed to construct 'PointerEvent'");
    init = init || {};
    MouseEvent.call(this, type, init);
    this.pointerId = init.pointerId == null ? 1 : numOrZero(init.pointerId);
    this.pointerType = init.pointerType || 'mouse';
    this.isPrimary = init.isPrimary !== false;
    this.width = numOrZero(init.width) || 1;
    this.height = numOrZero(init.height) || 1;
  }
  PointerEvent.prototype = Object.create(MouseEvent.prototype);
  PointerEvent.prototype.constructor = PointerEvent;

  function KeyboardEvent(type, init) {
    if (!(this instanceof KeyboardEvent)) throw new TypeError("Failed to construct 'KeyboardEvent'");
    init = init || {};
    UIEvent.call(this, type, init);
    this.key = init.key == null ? '' : String(init.key);
    this.code = init.code == null ? '' : String(init.code);
    this.location = numOrZero(init.location);
    this.repeat = !!init.repeat;
    this.ctrlKey = !!init.ctrlKey;
    this.shiftKey = !!init.shiftKey;
    this.altKey = !!init.altKey;
    this.metaKey = !!init.metaKey;
    this.charCode = numOrZero(init.charCode);
    this.keyCode = numOrZero(init.keyCode);
    this.which = numOrZero(init.which || init.keyCode);
  }
  KeyboardEvent.prototype = Object.create(UIEvent.prototype);
  KeyboardEvent.prototype.constructor = KeyboardEvent;

  function FocusEvent(type, init) {
    if (!(this instanceof FocusEvent)) throw new TypeError("Failed to construct 'FocusEvent'");
    init = init || {};
    UIEvent.call(this, type, init);
    this.relatedTarget = init.relatedTarget || null;
  }
  FocusEvent.prototype = Object.create(UIEvent.prototype);
  FocusEvent.prototype.constructor = FocusEvent;

  function InputEvent(type, init) {
    if (!(this instanceof InputEvent)) throw new TypeError("Failed to construct 'InputEvent'");
    init = init || {};
    UIEvent.call(this, type, init);
    this.data = init.data == null ? null : String(init.data);
    this.inputType = init.inputType == null ? '' : String(init.inputType);
  }
  InputEvent.prototype = Object.create(UIEvent.prototype);
  InputEvent.prototype.constructor = InputEvent;

  function SubmitEvent(type, init) {
    if (!(this instanceof SubmitEvent)) throw new TypeError("Failed to construct 'SubmitEvent'");
    init = init || {};
    Event.call(this, type, init);
    this.submitter = init.submitter || null;
  }
  SubmitEvent.prototype = Object.create(Event.prototype);
  SubmitEvent.prototype.constructor = SubmitEvent;

  function CustomEvent(type, init) {
    if (!(this instanceof CustomEvent)) throw new TypeError("Failed to construct 'CustomEvent'");
    init = init || {};
    Event.call(this, type, init);
    this.detail = init.detail === undefined ? null : init.detail;
  }
  CustomEvent.prototype = Object.create(Event.prototype);
  CustomEvent.prototype.constructor = CustomEvent;

  function MessageEvent(type, init) {
    if (!(this instanceof MessageEvent)) throw new TypeError("Failed to construct 'MessageEvent'");
    init = init || {};
    Event.call(this, type, init);
    this.data = init.data;
    this.origin = init.origin || '';
    this.lastEventId = init.lastEventId || '';
    this.source = init.source || null;
    this.ports = init.ports || [];
  }
  MessageEvent.prototype = Object.create(Event.prototype);
  MessageEvent.prototype.constructor = MessageEvent;

  function HashChangeEvent(type, init) {
    if (!(this instanceof HashChangeEvent)) throw new TypeError("Failed to construct 'HashChangeEvent'");
    init = init || {};
    Event.call(this, type, init);
    this.oldURL = init.oldURL || '';
    this.newURL = init.newURL || '';
  }
  HashChangeEvent.prototype = Object.create(Event.prototype);
  HashChangeEvent.prototype.constructor = HashChangeEvent;

  function PopStateEvent(type, init) {
    if (!(this instanceof PopStateEvent)) throw new TypeError("Failed to construct 'PopStateEvent'");
    init = init || {};
    Event.call(this, type, init);
    this.state = init.state === undefined ? null : init.state;
  }
  PopStateEvent.prototype = Object.create(Event.prototype);
  PopStateEvent.prototype.constructor = PopStateEvent;

  function makeEvent(type, init) { return new Event(type, init || {}); }

  function invokeListeners(store, event, phase, current) {
    if (!store || !store.list) return;
    var entries = store.list.filter(function (entry) {
      if (entry.removed || entry.type !== event.type) return false;
      if (phase === CAPTURING_PHASE) return entry.capture;
      if (phase === BUBBLING_PHASE) return !entry.capture;
      return true;
    });
    for (var i = 0; i < entries.length; ++i) {
      if (event.__stopImmediate) return;
      var entry = entries[i];
      if (entry.removed) continue;
      if (entry.once) entry.removed = true;
      event.eventPhase = phase;
      event.currentTarget = current;
      var previousPassive = event.__passive;
      event.__passive = entry.passive;
      try { entry.fn.call(current === undefined ? G : current, event); }
      catch (err) { reportError(err); }
      event.__passive = previousPassive;
      if (event.__stopImmediate) break;
    }
    store.list = store.list.filter(function (entry) { return !entry.removed; });
  }

  function storeOf(node) {
    if (!node) return null;
    if (node === document) return documentTarget;
    if (node === G) return windowTarget;
    if (node.__h) return storeFor(node.__h);
    if (node.__listeners instanceof ListenerStore) return node.__listeners;
    return null;
  }

  function eventPath(target) {
    var path = [];
    if (target && target.__h) {
      var node = target;
      while (node && node.__h) {
        path.push(node);
        var parent = H.parentNode(node.__h);
        node = parent ? wrap(parent) : null;
      }
      path.push(document);
      path.push(G);
      return path;
    }
    if (target === document) return [document, G];
    if (target === G) return [G];
    return [target];
  }

  function dispatchDOMEvent(target, event) {
    if (!event || !event.type) return true;
    if (!(event instanceof Event)) event = new Event(event.type, event);
    var path = eventPath(target);
    event.__path = path.slice();
    event.target = path[0];
    event.srcElement = path[0];
    event.__stop = false;
    event.__stopImmediate = false;
    for (var i = path.length - 1; i >= 1; --i) {
      if (event.__stop) break;
      invokeListeners(storeOf(path[i]), event, CAPTURING_PHASE, path[i]);
    }
    if (!event.__stop) invokeListeners(storeOf(path[0]), event, AT_TARGET, path[0]);
    if (event.bubbles && !event.__stop) {
      for (var j = 1; j < path.length; ++j) {
        if (event.__stop) break;
        invokeListeners(storeOf(path[j]), event, BUBBLING_PHASE, path[j]);
      }
    }
    event.eventPhase = 0;
    event.currentTarget = null;
    return !event.defaultPrevented;
  }

  function fireStored(store, event, target) {
    var ev = event instanceof Event ? event : new Event(event && event.type || 'event', event || {});
    ev.target = target;
    invokeListeners(store, ev, AT_TARGET, target);
  }

  function DOMException(message, name) {
    this.message = String(message || '');
    this.name = String(name || 'Error');
    this.code = 0;
  }
  DOMException.prototype = Object.create(Error.prototype);
  DOMException.prototype.constructor = DOMException;

  function AbortSignal() {
    this.aborted = false;
    this.reason = undefined;
    this.onabort = null;
    this.__listeners = new ListenerStore();
  }
  AbortSignal.prototype.addEventListener = function (type, fn, opts) {
    this.__listeners.add(type, fn, opts);
  };
  AbortSignal.prototype.removeEventListener = function (type, fn, opts) {
    this.__listeners.remove(type, fn, opts);
  };
  AbortSignal.prototype.throwIfAborted = function () {
    if (this.aborted) throw this.reason;
  };
  function AbortController() { this.signal = new AbortSignal(); }
  AbortController.prototype.abort = function (reason) {
    var signal = this.signal;
    if (signal.aborted) return;
    signal.aborted = true;
    signal.reason = reason === undefined
      ? new DOMException('The operation was aborted.', 'AbortError') : reason;
    var event = new Event('abort');
    event.target = signal;
    if (typeof signal.onabort === 'function') {
      try { signal.onabort.call(signal, event); } catch (err) { reportError(err); }
    }
    invokeListeners(signal.__listeners, event, AT_TARGET, signal);
  };

  var mutationObservers = [];
  var mutationScheduled = false;
  function MutationRecord(type, target, extra) {
    extra = extra || {};
    this.type = type;
    this.target = target;
    this.addedNodes = extra.addedNodes || [];
    this.removedNodes = extra.removedNodes || [];
    this.previousSibling = extra.previousSibling || null;
    this.nextSibling = extra.nextSibling || null;
    this.attributeName = extra.attributeName || null;
    this.attributeNamespace = null;
    this.oldValue = extra.oldValue === undefined ? null : extra.oldValue;
  }
  function MutationObserver(callback) {
    if (typeof callback !== 'function') throw new TypeError('MutationObserver requires a callback');
    this.callback = callback;
    this.__records = [];
    this.__registrations = [];
  }
  MutationObserver.prototype.observe = function (target, options) {
    options = options || {};
    if (!options.childList && !options.attributes && !options.characterData) {
      throw new TypeError('MutationObserver options require childList, attributes, or characterData');
    }
    this.__registrations = this.__registrations.filter(function (reg) { return reg.target !== target; });
    this.__registrations.push({ target: target, options: options });
    if (mutationObservers.indexOf(this) < 0) mutationObservers.push(this);
  };
  MutationObserver.prototype.disconnect = function () {
    this.__registrations = [];
    this.__records = [];
    var self = this;
    mutationObservers = mutationObservers.filter(function (observer) { return observer !== self; });
  };
  MutationObserver.prototype.takeRecords = function () {
    var records = this.__records.slice();
    this.__records = [];
    return records;
  };
  function observerWants(observer, target, type, attrName) {
    for (var i = 0; i < observer.__registrations.length; ++i) {
      var reg = observer.__registrations[i];
      var node = reg.target;
      var opts = reg.options || {};
      var hit = node === target;
      if (!hit && opts.subtree) {
        if (typeof node.contains === 'function' && target) hit = node.contains(target);
        else if (node === document && target && target.__h) hit = isConnectedHandle(target.__h);
      }
      if (!hit) continue;
      if (type === 'childList' && opts.childList) return opts;
      if (type === 'characterData' && opts.characterData) return opts;
      if (type === 'attributes' && opts.attributes) {
        if (!opts.attributeFilter) return opts;
        var name = String(attrName || '').toLowerCase();
        for (var a = 0; a < opts.attributeFilter.length; ++a) {
          if (String(opts.attributeFilter[a]).toLowerCase() === name) return opts;
        }
      }
    }
    return null;
  }
  function deliverMutations() {
    mutationScheduled = false;
    var pending = mutationObservers.slice();
    pending.forEach(function (observer) {
      if (!observer.__records.length) return;
      var records = observer.takeRecords();
      try { observer.callback.call(observer, records, observer); }
      catch (err) { reportError(err); }
    });
  }
  function notifyMutation(type, target, extra) {
    var queued = false;
    mutationObservers.forEach(function (observer) {
      var opts = observerWants(observer, target, type, extra && extra.attributeName);
      if (!opts) return;
      var recordExtra = extra || {};
      var keepOld = (type === 'attributes' && opts.attributeOldValue) ||
        (type === 'characterData' && opts.characterDataOldValue);
      observer.__records.push(new MutationRecord(type, target, {
        addedNodes: recordExtra.addedNodes,
        removedNodes: recordExtra.removedNodes,
        previousSibling: recordExtra.previousSibling,
        nextSibling: recordExtra.nextSibling,
        attributeName: recordExtra.attributeName,
        oldValue: keepOld ? recordExtra.oldValue : null
      }));
      queued = true;
    });
    if (!queued || mutationScheduled) return;
    mutationScheduled = true;
    if (typeof queueMicrotask === 'function') queueMicrotask(deliverMutations);
  }

  function bindOneInline(el, name, value) {
    if (!el || !el.__h) return;
    var lower = String(name).toLowerCase();
    if (lower.indexOf('on') !== 0 || lower.length < 3) return;
    var type = lower.slice(2);
    if (!el.__inline) el.__inline = {};
    if (el.__inline[type]) {
      storeFor(el.__h).remove(type, el.__inline[type], false);
      el.__inline[type] = null;
    }
    if (value == null || value === '') return;
    var fn;
    try { fn = new Function('event', String(value)); }
    catch (err) { return; }
    var bound = function (event) { return fn.call(el, event); };
    el.__inline[type] = bound;
    storeFor(el.__h).add(type, bound, false);
  }
  function bindInlineHandlers(wrapper) {
    if (!wrapper || !wrapper.__h || H.nodeType(wrapper.__h) !== 1) return;
    var attrs = H.attrs(wrapper.__h);
    for (var i = 0; i < attrs.length; ++i) bindOneInline(wrapper, attrs[i][0], attrs[i][1]);
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
    bindInlineHandlers(wrapper);
    return wrapper;
  }

  function ElementNode() {}
  function TextNode() {}
  function CommentNode() {}

  function attrString(el, name) { return H.attr(el.__h, name); }
  function setAttrString(el, name, value) {
    var previous = H.attr(el.__h, name);
    H.setAttr(el.__h, name, String(value));
    notifyMutation('attributes', el, { attributeName: String(name), oldValue: previous });
    bindOneInline(el, name, value);
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
    dispatchDOMEvent(el, new Event(type));
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
        var kind = H.nodeType(this.__h);
        if (kind === 3 || kind === 8) {
          var previous = H.text(this.__h);
          H.setText(this.__h, String(v));
          notifyMutation('characterData', this, { oldValue: previous });
          return;
        }
        var removed = this.childNodes.slice();
        H.setText(this.__h, String(v));
        notifyMutation('childList', this, { removedNodes: removed, addedNodes: this.childNodes.slice() });
      }, enumerable: true
    },
    innerText: {
      get: function () { return this.textContent; },
      set: function (v) { this.textContent = v; }
    },
    innerHTML: {
      get: function () { return H.innerHTML(this.__h); },
      set: function (v) {
        var removed = this.childNodes.slice();
        H.setInnerHTML(this.__h, String(v));
        notifyMutation('childList', this, { removedNodes: removed, addedNodes: this.childNodes.slice() });
      }, enumerable: true
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
        if (tag === 'textarea') this.textContent = String(v);
        else {
          if (tag === 'option' && H.attr(this.__h, 'value') == null) H.setText(this.__h, String(v));
          setAttrString(this, 'value', v);
        }
        if (this.__settingValue) return;
        if (tag !== 'input' && tag !== 'textarea' && tag !== 'select') return;
        this.__settingValue = true;
        try {
          dispatchDOMEvent(this, new InputEvent('input', {
            bubbles: true, data: String(v), inputType: 'insertReplacementText'
          }));
        } catch (err) { reportError(err); }
        this.__settingValue = false;
      }, enumerable: true
    });
  }
  defineValueProperty(ElementNode.prototype);

  function defineCheckedProperty(proto) {
    Object.defineProperty(proto, 'checked', {
      get: function () { return H.hasAttr(this.__h, 'checked'); },
      set: function (v) {
        if (v) setAttrString(this, 'checked', 'checked');
        else {
          var previous = H.attr(this.__h, 'checked');
          H.delAttr(this.__h, 'checked');
          notifyMutation('attributes', this, { attributeName: 'checked', oldValue: previous });
        }
        if (H.tagName(this.__h) !== 'input' || this.__settingValue) return;
        this.__settingValue = true;
        try {
          dispatchDOMEvent(this, new Event('input', { bubbles: true }));
          dispatchDOMEvent(this, new Event('change', { bubbles: true }));
        } catch (err) { reportError(err); }
        this.__settingValue = false;
      }
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
  ElementNode.prototype.removeAttribute = function (name) {
    var previous = H.attr(this.__h, String(name));
    H.delAttr(this.__h, String(name));
    notifyMutation('attributes', this, { attributeName: String(name), oldValue: previous });
    bindOneInline(this, name, '');
  };
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
      notifyMutation('childList', this, { addedNodes: [child] });
      maybeExecuteScript(child);
      return child;
    }
    if (child && child.__fragment) {
      var added = [];
      child.__nodes.forEach(function (h) {
        H.appendChild(this.__h, h);
        var wrapped = wrap(h);
        added.push(wrapped);
        maybeExecuteScript(wrapped);
      }, this);
      notifyMutation('childList', this, { addedNodes: added });
      child.__nodes = [];
      return child;
    }
    return child;
  };
  ElementNode.prototype.insertBefore = function (node, ref) {
    if (node && node.__h) {
      H.insertBefore(this.__h, node.__h, ref ? ref.__h : 0);
      notifyMutation('childList', this, { addedNodes: [node], nextSibling: ref || null });
      maybeExecuteScript(node);
      return node;
    }
    return node;
  };
  ElementNode.prototype.removeChild = function (child) {
    if (child && child.__h) {
      H.detach(child.__h);
      notifyMutation('childList', this, { removedNodes: [child] });
    }
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
  ElementNode.prototype.removeEventListener = function (type, fn, opts) { storeFor(this.__h).remove(String(type), fn, opts); };
  ElementNode.prototype.dispatchEvent = function (event) {
    if (!event || !event.type) return true;
    var ev = event instanceof Event ? event : new Event(event.type, event);
    var proceed = dispatchDOMEvent(this, ev);
    if (proceed) runDefaultAction(this, ev);
    return proceed;
  };
  ElementNode.prototype.dispatch = function (type, init) {
    return this.dispatchEvent(new Event(type, init || {}));
  };
  ElementNode.prototype.focus = function () {
    var previous = document.activeElement;
    document.activeElement = this;
    if (previous && previous !== this && previous.__h) {
      dispatchDOMEvent(previous, new FocusEvent('blur', { relatedTarget: this }));
      dispatchDOMEvent(previous, new FocusEvent('focusout', { bubbles: true, relatedTarget: this }));
    }
    dispatchDOMEvent(this, new FocusEvent('focus', { relatedTarget: previous || null }));
    dispatchDOMEvent(this, new FocusEvent('focusin', { bubbles: true, relatedTarget: previous || null }));
  };
  ElementNode.prototype.blur = function () {
    if (document.activeElement !== this) return;
    document.activeElement = document.body || null;
    dispatchDOMEvent(this, new FocusEvent('blur'));
    dispatchDOMEvent(this, new FocusEvent('focusout', { bubbles: true }));
  };
  ElementNode.prototype.click = function () {
    if (!isInteractable(this)) {
      return false;
    }
    var clickEvent = dispatchClickCascade(this);
    if (clickEvent) runDefaultAction(this, clickEvent);
    try { __prowsetkFlush(); } catch (e) { reportError(e); }
    return !!clickEvent && !clickEvent.defaultPrevented;
  };

  ElementNode.prototype.type = function (text) {
    if (!isInteractable(this)) {
      return false;
    }
    var proceed = dispatchTypeCascade(this, String(text == null ? '' : text));
    try { __prowsetkFlush(); } catch (e) { reportError(e); }
    return proceed;
  };

  // Layout-free interactability heuristics (README "Synthetic Interaction
  // Driver & SPA Event Cascades" section 3). An element is non-interactable
  // if it or any ancestor has display:none, visibility:hidden, hidden attribute,
  // or disabled/aria-disabled="true".
  function isInteractable(element) {
    if (!element || !element.__h) return false;
    var node = element;
    while (node && node.__h) {
      var style = H.attr(node.__h, 'style') || '';
      if (/\bdisplay\s*:\s*none\b/.test(style)) return false;
      if (/\bvisibility\s*:\s*hidden\b/.test(style)) return false;
      if (H.hasAttr(node.__h, 'hidden')) return false;
      if (H.hasAttr(node.__h, 'disabled')) return false;
      if (H.attr(node.__h, 'aria-disabled') === 'true') return false;
      var parent = H.parentNode(node.__h);
      node = parent ? wrap(parent) : null;
    }
    return true;
  }

  // Native prototype setter bypass for controlled inputs (React/Vue/etc.).
  // See README "Synthetic Interaction Driver & SPA Event Cascades" section 2.
  // The shim's `value` setter emits its own `input` event; suppress it here so
  // the type cascade below emits exactly one canonical `input`
  // (`inputType: 'insertText'`) per character.
  function __prowsetkSetValue(element, value) {
    var proto = Object.getPrototypeOf(element);
    var descriptor = Object.getOwnPropertyDescriptor(proto, 'value');
    element.__settingValue = true;
    try {
      if (descriptor && descriptor.set) {
        descriptor.set.call(element, value);
      } else {
        element.value = value;
      }
    } finally {
      element.__settingValue = false;
    }
  }

  // Pointer & Click Cascade Contract (README section 1).
  // Dispatches: pointerover, pointerenter, pointerdown, mousedown, focus,
  // pointerup, mouseup, click. Returns the click event, or null when an
  // earlier cancelable stage was canceled. Submit handling is left to the
  // click default action (`runDefaultAction` -> `requestSubmitForm`), which
  // fires a cancelable `submit` on the enclosing form only when the click
  // itself was not canceled.
  function dispatchClickCascade(element) {
    var pointerId = 1;
    var commonInit = { bubbles: true, cancelable: true, composed: true,
                       pointerId: pointerId, pointerType: 'mouse', isPrimary: true,
                       button: 0, buttons: 1, clientX: 0, clientY: 0 };
    var enterInit = { bubbles: false, cancelable: false,
                      pointerId: pointerId, pointerType: 'mouse', isPrimary: true };
    var focusInit = { relatedTarget: null };
    var upInit = { bubbles: true, cancelable: true, composed: true,
                   pointerId: pointerId, pointerType: 'mouse', isPrimary: true,
                   button: 0, buttons: 0, clientX: 0, clientY: 0 };
    var clickInit = { bubbles: true, cancelable: true, composed: true, button: 0 };

    // 1. pointerover
    if (!dispatchDOMEvent(element, new PointerEvent('pointerover', commonInit))) return null;
    // 2. pointerenter
    dispatchDOMEvent(element, new PointerEvent('pointerenter', enterInit));
    // 3. pointerdown
    if (!dispatchDOMEvent(element, new PointerEvent('pointerdown', commonInit))) return null;
    // 4. mousedown
    if (!dispatchDOMEvent(element, new MouseEvent('mousedown', commonInit))) return null;
    // 5. focus (if focusable and not already active)
    var isFocusable = (function (el) {
      var tag = H.tagName(el.__h);
      var type = (el.getAttribute('type') || '').toLowerCase();
      if (tag === 'a' || tag === 'button' || tag === 'input' || tag === 'select' ||
          tag === 'textarea' || el.hasAttribute('tabindex') || el.hasAttribute('contenteditable')) {
        return true;
      }
      return false;
    })(element);
    if (isFocusable && document.activeElement !== element) {
      var prevActive = document.activeElement;
      document.activeElement = element;
      if (prevActive && prevActive !== element && prevActive.__h) {
        dispatchDOMEvent(prevActive, new FocusEvent('blur', focusInit));
        dispatchDOMEvent(prevActive, new FocusEvent('focusout', { bubbles: true, relatedTarget: element }));
      }
      dispatchDOMEvent(element, new FocusEvent('focus', focusInit));
      dispatchDOMEvent(element, new FocusEvent('focusin', { bubbles: true, relatedTarget: prevActive || null }));
    }
    // 6. pointerup
    if (!dispatchDOMEvent(element, new PointerEvent('pointerup', upInit))) return null;
    // 7. mouseup
    if (!dispatchDOMEvent(element, new MouseEvent('mouseup', upInit))) return null;
    // 8. click
    var clickEvent = new MouseEvent('click', clickInit);
    dispatchDOMEvent(element, clickEvent);
    return clickEvent;
  }

  // Controlled Input & Keyboard Cascade (README section 2).
  // For each character/chunk: focus, keydown, keypress (if printable),
  // update value via __prowsetkSetValue, input, keyup. Then change and blur.
  function dispatchTypeCascade(element, text) {
    var tag = H.tagName(element.__h);
    var type = (element.getAttribute('type') || '').toLowerCase();
    var isInput = tag === 'input' || tag === 'textarea' || element.hasAttribute('contenteditable');
    if (!isInput) return false;

    // Focus first
    var focusInit = { relatedTarget: null };
    var prevActive = document.activeElement;
    document.activeElement = element;
    if (prevActive && prevActive !== element && prevActive.__h) {
      dispatchDOMEvent(prevActive, new FocusEvent('blur', focusInit));
      dispatchDOMEvent(prevActive, new FocusEvent('focusout', { bubbles: true, relatedTarget: element }));
    }
    dispatchDOMEvent(element, new FocusEvent('focus', focusInit));
    dispatchDOMEvent(element, new FocusEvent('focusin', { bubbles: true, relatedTarget: prevActive || null }));

    // Type each character
    for (var i = 0; i < text.length; ++i) {
      var ch = text.charAt(i);
      var code = ch.charCodeAt(0);
      var key = ch;
      var keyCode = code;
      var which = code;
      var isPrintable = code >= 32 && code <= 126;

      // keydown
      var keyInit = { key: key, code: 'Key' + key.toUpperCase(), bubbles: true,
                      ctrlKey: false, shiftKey: false, altKey: false, metaKey: false,
                      charCode: isPrintable ? code : 0, keyCode: keyCode, which: which };
      dispatchDOMEvent(element, new KeyboardEvent('keydown', keyInit));
      // keypress (if printable)
      if (isPrintable) {
        dispatchDOMEvent(element, new KeyboardEvent('keypress', keyInit));
      }
      // Update value via native setter
      var currentValue = element.value || '';
      var newValue = currentValue + ch;
      __prowsetkSetValue(element, newValue);
      // input event
      dispatchDOMEvent(element, new InputEvent('input', { bubbles: true, composed: true,
                           inputType: 'insertText', data: ch }));
      // keyup
      dispatchDOMEvent(element, new KeyboardEvent('keyup', keyInit));
    }

    // change event on commit
    dispatchDOMEvent(element, new Event('change', { bubbles: true, cancelable: false }));

    // blur (simulate focus lost after typing)
    document.activeElement = document.body || null;
    dispatchDOMEvent(element, new FocusEvent('blur'));
    dispatchDOMEvent(element, new FocusEvent('focusout', { bubbles: true }));

    return true;
  }

  ElementNode.prototype.getBoundingClientRect = function () {
    return { x: 0, y: 0, top: 0, left: 0, right: 0, bottom: 0, width: 0, height: 0 };
  };
  ElementNode.prototype.requestSubmit = function (submitter) {
    var form = H.tagName(this.__h) === 'form' ? this : (this.closest ? this.closest('form') : null);
    if (form) requestSubmitForm(form, submitter || null);
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

  function performFormSubmit(form) {
    if (!form || H.tagName(form.__h) !== 'form') return;
    var method = (form.getAttribute('method') || 'GET').toUpperCase();
    var action = form.action || H.pageInfo().url;
    var fields = formFields(form);
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
  }
  function requestSubmitForm(form, submitter) {
    var event = new SubmitEvent('submit', { bubbles: true, cancelable: true, submitter: submitter || null });
    if (!dispatchDOMEvent(form, event)) return;
    performFormSubmit(form);
  }
  function runDefaultAction(target, event) {
    if (!target || !target.__h || !event || event.defaultPrevented) return;
    var tag = H.tagName(target.__h);
    if (event.type === 'click') {
      if (tag === 'a' || tag === 'area') {
        var href = target.href;
        if (href && href.indexOf('javascript:') !== 0) navigateTo(href);
        return;
      }
      var controlType = (target.getAttribute('type') || (tag === 'button' ? 'submit' : '')).toLowerCase();
      if ((tag === 'button' || tag === 'input') && controlType === 'submit') {
        var form = target.form || (target.closest ? target.closest('form') : null);
        if (form) requestSubmitForm(form, target);
        return;
      }
      if ((tag === 'button' || tag === 'input') && controlType === 'reset') {
        var resetForm = target.closest ? target.closest('form') : null;
        if (resetForm && resetForm.reset) resetForm.reset();
      }
    }
  }
  ElementNode.prototype.submit = function () {
    performFormSubmit(this);
  };
  var elementEventTypes = ['click', 'dblclick', 'mousedown', 'mouseup', 'mouseover', 'mouseout',
    'mousemove', 'pointerdown', 'pointerup', 'pointermove', 'input', 'change', 'submit', 'reset',
    'focus', 'blur', 'focusin', 'focusout', 'keydown', 'keyup', 'keypress', 'load', 'error',
    'contextmenu'];
  elementEventTypes.forEach(function (type) {
    Object.defineProperty(ElementNode.prototype, 'on' + type, {
      configurable: true,
      get: function () { return (this.__on && this.__on[type]) || null; },
      set: function (fn) {
        if (!this.__on) this.__on = {};
        if (this.__on[type]) storeFor(this.__h).remove(type, this.__on[type], false);
        this.__on[type] = typeof fn === 'function' ? fn : null;
        if (this.__on[type]) storeFor(this.__h).add(type, this.__on[type], false);
      }
    });
  });
  Object.defineProperty(ElementNode.prototype, 'method', {
    get: function () {
      if (H.tagName(this.__h) !== 'form') return '';
      var value = (this.getAttribute('method') || 'get').toLowerCase();
      return value === 'post' || value === 'dialog' ? value : 'get';
    },
    set: function (value) { setAttrString(this, 'method', value); }
  });
  Object.defineProperty(ElementNode.prototype, 'form', {
    get: function () {
      var tag = H.tagName(this.__h);
      if (tag === 'form') return this;
      var id = H.attr(this.__h, 'form');
      if (id) {
        var owner = document.getElementById(id);
        if (owner && H.tagName(owner.__h) === 'form') return owner;
      }
      return this.closest ? this.closest('form') : null;
    }
  });
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
  DOMInterfaces.EventTarget = interfaceCtor('EventTarget', Object.prototype);
  DOMInterfaces.Node = interfaceCtor('Node', DOMInterfaces.EventTarget.prototype);
  DOMInterfaces.Document = interfaceCtor('Document', DOMInterfaces.Node.prototype);
  DOMInterfaces.Window = interfaceCtor('Window', DOMInterfaces.EventTarget.prototype);
  [['ELEMENT_NODE', 1], ['ATTRIBUTE_NODE', 2], ['TEXT_NODE', 3],
   ['CDATA_SECTION_NODE', 4], ['PROCESSING_INSTRUCTION_NODE', 7],
   ['COMMENT_NODE', 8], ['DOCUMENT_NODE', 9], ['DOCUMENT_TYPE_NODE', 10],
   ['DOCUMENT_FRAGMENT_NODE', 11]].forEach(function (pair) {
    DOMInterfaces.Node[pair[0]] = pair[1];
    DOMInterfaces.Node.prototype[pair[0]] = pair[1];
  });
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
    createEvent: function () { return new Event(''); },
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
    removeEventListener: function (type, fn, opts) { documentTarget.remove(String(type), fn, opts); },
    dispatchEvent: function (event) {
      if (!event || !event.type) return true;
      var ev = event instanceof Event ? event : new Event(event.type, event);
      return dispatchDOMEvent(document, ev);
    },
    contains: function (other) {
      if (other === document) return true;
      if (!other || !other.__h) return false;
      return isConnectedHandle(other.__h);
    },
    get forms() { return H.queryAll('form').map(function (h) { return wrap(h); }); },
    elementFromPoint: function () { return null; },
    getSelection: function () { return { toString: function () { return ''; }, removeAllRanges: function () {} }; },
    createRange: function () { return { selectNodeContents: function () {}, toString: function () { return ''; } }; },
    createTreeWalker: function () { return { nextNode: function () { return null; } }; },
    createNodeIterator: function () { return { nextNode: function () { return null; } }; }
  };
  document.addEventListener = function (type, fn, opts) { documentTarget.add(String(type), fn, opts); };
  document.removeEventListener = function (type, fn, opts) { documentTarget.remove(String(type), fn, opts); };
  document.dispatchEvent = function (event) {
    if (!event || !event.type) return true;
    var ev = event instanceof Event ? event : new Event(event.type, event);
    return dispatchDOMEvent(document, ev);
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
    var current = parseUrl(currentHref(), null);
    var sameDocument = current && resolved.protocol === current.protocol &&
      resolved.host === current.host && resolved.pathname === current.pathname &&
      resolved.search === current.search;
    locationState.override = resolved.href;
    if (sameDocument) {
      if (resolved.hash !== current.hash) {
        dispatchDOMEvent(G, new HashChangeEvent('hashchange', {
          bubbles: false, oldURL: current.href, newURL: resolved.href
        }));
      }
      return;
    }
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
      dispatchDOMEvent(document, new Event('DOMContentLoaded'));
      dispatchDOMEvent(document, new Event('readystatechange'));
      return 1;
    }
    if (name === 'load') {
      docState.readyState = 'complete';
      dispatchDOMEvent(G, new Event('load'));
      dispatchDOMEvent(document, new Event('readystatechange'));
      return 1;
    }
    return 0;
  }

  function __prowsetkFlush() {
    var fired = 0;
    while (fired < MAX_FLUSH_CALLBACKS) {
      var ran = 0;
      if (lifecycleQueue.length) ran += fireLifecycle(lifecycleQueue.shift());
      var jobs = 0;
      try { jobs = Number(H.drainJobs()) || 0; } catch (err) { reportError(err); }
      if (ran || jobs) { fired += ran + jobs; continue; }
      var now = Date.now();
      var due = null;
      for (var i = 0; i < timers.length; ++i) {
        if (timers[i].active !== false && timers[i].when <= now && (!due || due.when > timers[i].when)) due = timers[i];
      }
      if (rafCbs.length && !due) due = { when: now, fn: function () { var snapshot = rafCbs; rafCbs = []; snapshot.forEach(function (r) { if (r.fn) { try { r.fn(now); } catch (e) { reportError(e); } } }); }, args: [], interval: 0, id: 0 };
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
    if (!this.__listenerStore) this.__listenerStore = new ListenerStore();
    this.__listenerStore.add(type, fn, opts);
  };
  XMLHttpRequest.prototype.removeEventListener = function (type, fn, opts) {
    if (!this.__listenerStore) this.__listenerStore = new ListenerStore();
    this.__listenerStore.remove(type, fn, opts);
  };
  XMLHttpRequest.prototype.dispatchEvent = function (ev) {
    if (!ev || !ev.type) return true;
    this.__fire(ev.type, ev);
    return !ev.defaultPrevented;
  };
  XMLHttpRequest.prototype.__fire = function (type, event) {
    var ev = event instanceof Event ? event : new Event(type);
    ev.target = this;
    if (this.__listenerStore) invokeListeners(this.__listenerStore, ev, AT_TARGET, this);
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

  function ObserverStub() {
    function ObserverCtor(callback) {
      this.callback = callback;
      this.__observed = [];
    }
    ObserverCtor.prototype.observe = function (target, opts) { this.__observed.push([target, opts || {}]); };
    ObserverCtor.prototype.unobserve = function () {};
    ObserverCtor.prototype.disconnect = function () { this.__observed = []; };
    ObserverCtor.prototype.takeRecords = function () { return []; };
    return ObserverCtor;
  }
  function MessagePort() {
    this.onmessage = null;
    this.__listeners = new ListenerStore();
    this.__other = null;
    this.__closed = false;
  }
  MessagePort.prototype.addEventListener = function (type, fn, opts) { this.__listeners.add(type, fn, opts); };
  MessagePort.prototype.removeEventListener = function (type, fn, opts) { this.__listeners.remove(type, fn, opts); };
  MessagePort.prototype.start = function () {};
  MessagePort.prototype.close = function () { this.__closed = true; };
  MessagePort.prototype.postMessage = function (data) {
    var other = this.__other;
    if (!other || other.__closed || this.__closed) return;
    setTimeoutJs(function () {
      if (other.__closed) return;
      var event = new MessageEvent('message', { data: data });
      event.target = other;
      if (typeof other.onmessage === 'function') {
        try { other.onmessage.call(other, event); } catch (err) { reportError(err); }
      }
      invokeListeners(other.__listeners, event, AT_TARGET, other);
    }, 0);
  };
  function MessageChannel() {
    this.port1 = new MessagePort();
    this.port2 = new MessagePort();
    this.port1.__other = this.port2;
    this.port2.__other = this.port1;
  }

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
  install('Event', Event, true);
  install('CustomEvent', CustomEvent, true);
  install('UIEvent', UIEvent, true);
  install('MouseEvent', MouseEvent, true);
  install('PointerEvent', PointerEvent, true);
  install('KeyboardEvent', KeyboardEvent, true);
  install('FocusEvent', FocusEvent, true);
  install('InputEvent', InputEvent, true);
  install('SubmitEvent', SubmitEvent, true);
  install('MessageEvent', MessageEvent, true);
  install('HashChangeEvent', HashChangeEvent, true);
  install('PopStateEvent', PopStateEvent, true);
  install('DOMException', DOMException, true);
  install('AbortController', AbortController, true);
  install('AbortSignal', AbortSignal, true);
  install('MessageChannel', MessageChannel, true);
  install('MessagePort', MessagePort, true);
  install('DOMParser', DOMParserStub, true);
  install('FormData', FormDataStub, true);
  install('MutationObserver', MutationObserver, true);
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
  var historyEntries = [{ state: null, url: null }];
  var historyIndex = 0;
  install('history', {
    get length() { return historyEntries.length; },
    get state() { return historyEntries[historyIndex] ? historyEntries[historyIndex].state : null; },
    scrollRestoration: 'auto',
    pushState: function (state, title, url) {
      historyEntries = historyEntries.slice(0, historyIndex + 1);
      var nextUrl = url == null ? currentHref() : parseUrl(url, null).href;
      if (nextUrl && nextUrl.indexOf('javascript:') === 0) return;
      historyEntries.push({ state: state, url: nextUrl });
      historyIndex = historyEntries.length - 1;
      if (url != null) locationState.override = nextUrl;
    },
    replaceState: function (state, title, url) {
      var nextUrl = url == null ? currentHref() : parseUrl(url, null).href;
      if (nextUrl && nextUrl.indexOf('javascript:') === 0) return;
      historyEntries[historyIndex] = { state: state, url: nextUrl };
      if (url != null) locationState.override = nextUrl;
    },
    back: function () { this.go(-1); },
    forward: function () { this.go(1); },
    go: function (delta) {
      var next = historyIndex + (Number(delta) || 0);
      if (next < 0 || next >= historyEntries.length || next === historyIndex) return;
      historyIndex = next;
      var entry = historyEntries[historyIndex];
      if (entry.url) locationState.override = entry.url;
      dispatchDOMEvent(G, new PopStateEvent('popstate', { state: entry.state }));
    }
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
  G.removeEventListener = function (type, fn, opts) { windowTarget.remove(String(type), fn, opts); };
  G.dispatchEvent = function (event) {
    if (!event || !event.type) return true;
    var ev = event instanceof Event ? event : new Event(event.type, event);
    return dispatchDOMEvent(G, ev);
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

  function installOnProperty(target, store, type) {
    var slot = null;
    Object.defineProperty(target, 'on' + type, {
      configurable: true,
      enumerable: true,
      get: function () { return slot; },
      set: function (fn) {
        if (slot) store.remove(type, slot, false);
        slot = typeof fn === 'function' ? fn : null;
        if (slot) store.add(type, slot, false);
      }
    });
  }
  ['error', 'load', 'unload', 'beforeunload', 'hashchange', 'popstate', 'resize',
   'scroll', 'orientationchange'].forEach(function (type) {
    installOnProperty(G, windowTarget, type);
  });
  installOnProperty(document, documentTarget, 'readystatechange');
  try { Object.setPrototypeOf(document, DOMInterfaces.Document.prototype); } catch (err) {}
  try { Object.setPrototypeOf(G, DOMInterfaces.Window.prototype); } catch (err) {}
})();
)SHIM";

}  // namespace prowsetk

#endif  // PROWSETK_WEB_PLATFORM_SHIM_HPP
