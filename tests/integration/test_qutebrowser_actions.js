// Execute the actual generated fixed-action source in a minimal page model.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync(process.argv[2], 'utf8');
function run(origin, hidden = false) {
  const events = [], markers = [];
  let value = '';
  const proto = {};
  Object.defineProperty(proto, 'value', {set(v) { value = v; }});
  const input = Object.create(proto);
  Object.assign(input, {tagName: 'INPUT', type: 'text', parentElement: null,
    getAttribute() { return null; }, focus() { events.push('focus'); },
    dispatchEvent(e) { events.push(e.type); }});
  const context = {location: {origin}, getComputedStyle() { return {display: hidden ? 'none' : 'block'}; },
    Event: class {constructor(type) { this.type = type; }},
    document: {querySelector() { return input; }, querySelectorAll() { return []; },
      createElement() { return {setAttribute() {}}; }, head: {appendChild(e) { markers.push(e); }}}};
  vm.runInNewContext(source, context);
  assert.equal(context.injected, undefined);
  return {events, markers, value};
}
const success = run('https://admin.booking.com');
assert.equal(success.value, "'; injected=true; //");
assert.deepEqual(success.events, ['focus', 'input', 'change']);
assert.equal(success.markers[0].content, 'ok');
const outside = run('https://other.test');
assert.equal(outside.value, '');
assert.deepEqual(outside.events, []);
const hidden = run('https://admin.booking.com', true);
assert.equal(hidden.value, '');
assert.equal(hidden.markers[0].content, 'error');
