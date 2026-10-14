const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');

const appPath = path.join(__dirname, 'static', 'app.js');
const appSource = fs.readFileSync(appPath, 'utf8');

class Element {
  constructor(tag = 'div') {
    this.tagName = tag.toUpperCase();
    this.children = [];
    this.listeners = {};
    this.attributes = {};
    this.classList = {toggle() {}, add() {}};
    this.hidden = false;
    this.disabled = false;
    this.dataset = {};
    this._textContent = '';
    this._innerHTML = '';
  }
  set textContent(value) { this._textContent = String(value); this.children = []; }
  get textContent() { return this._textContent; }
  set innerHTML(value) { this._innerHTML = String(value); }
  get innerHTML() { return this._innerHTML; }
  append(...nodes) { this.children.push(...nodes); }
  replaceChildren(...nodes) { this.children = [...nodes]; }
  addEventListener(type, handler) { this.listeners[type] = handler; }
  setAttribute(name, value) { this.attributes[name] = String(value); }
  querySelector(selector) {
    if (selector === 'h3' || selector === 'p') {
      if (!this.nested) this.nested = {};
      return this.nested[selector] ||= new Element(selector);
    }
    return new Element();
  }
  focus() {}
}

async function createHarness(historyResponse) {
  const ids = new Map();
  const document = {
    getElementById(id) { return ids.get(id) || (ids.set(id, new Element()), ids.get(id)); },
    createElement(tag) { return new Element(tag); },
    querySelectorAll(selector) { return []; },
    querySelector(selector) { return new Element(); },
  };
  const context = {
    document,
    TextEncoder,
    console,
    Date,
    setInterval() {},
    fetch: async url => {
      if (url === '/api/history') {
        if (historyResponse instanceof Error) throw historyResponse;
        return {ok:true, status:200, json:async() => historyResponse};
      }
      if (url === '/api/state') {
        return {ok:true, status:200, json:async() => ({items:[], revision:0, transport:{mqtt_connected:false, published_revision:0, topic:'topic', mqtt_port:1883, device_connected:false, device_revision:0, broker_host:'localhost'}})};
      }
      throw new Error(`Unexpected URL: ${url}`);
    },
  };
  vm.createContext(context);
  vm.runInContext(appSource, context, {filename:appPath});
  await vm.runInContext('(async () => { await refreshHistory(); })()', context);
  return {context, ids};
}

(async () => {
  const payload = {events:[
    {source:'tablet', action:'update', item_id:'t-1', title_before:'Old title', title_after:'Tablet title', created_at:'2026-10-14T12:00:00Z', revision:2, outcome:'applied'},
    {source:'web', action:'create', item_id:'t-2', title_after:'<img src=x onerror=alert(1)>', created_at:'2026-10-14T12:01:00Z', revision:3, outcome:'applied'},
    {source:'tablet', action:'update', item_id:'t-3', title_before:'Conflicting task', created_at:'2026-10-14T12:02:00Z', revision:3, outcome:'conflict'},
    {source:'tablet', action:'delete', item_id:'t-4', title_before:'Rejected task', created_at:'2026-10-14T12:03:00Z', revision:3, outcome:'rejected'},
  ]};
  const {ids} = await createHarness(payload);
  const rows = ids.get('history-list').children;
  assert.equal(rows.length, 4);
  const rendered = rows.map(row => row.children.map(child => child.textContent).join(' '));
  assert.match(rendered[0], /Updated · Tablet title/);
  assert.match(rendered[0], /Came from tablet/);
  assert.match(rendered[1], /Created · <img src=x onerror=alert\(1\)>/);
  assert.match(rendered[1], /Came from web/);
  assert.match(rendered[2], /Conflict · Conflicting task/);
  assert.match(rendered[3], /Rejected · Rejected task/);
  assert.ok(rows.every(row => row.children.every(child => child.innerHTML === '')),
    'history must render all event text through textContent, never innerHTML');
  assert.equal(ids.get('history-empty').hidden, true);

  const failed = await createHarness(new Error('server unavailable'));
  assert.equal(failed.ids.get('history-empty').textContent, 'History is unavailable right now.');
  assert.equal(failed.ids.get('history-empty').hidden, false);
  console.log('History UI tests passed.');
})().catch(error => { console.error(error); process.exitCode = 1; });
