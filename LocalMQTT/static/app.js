const $ = id => document.getElementById(id);
let state = null, filter = 'all', editing = null, busy = false, renderedRevision = -1;
let pendingCreate = null;
const encoder = new TextEncoder();

function error(message) { $('error').textContent = message; $('error').hidden = !message; }
async function api(url, method = 'GET', body) {
  const response = await fetch(url, {method, headers: body ? {'Content-Type':'application/json'} : {}, body: body ? JSON.stringify(body) : undefined});
  const data = await response.json();
  if (!response.ok) throw new Error(data.error || `Request failed (${response.status})`);
  return data;
}
function button(label, handler, className = '') {
  const b = document.createElement('button'); b.type = 'button'; b.textContent = label;
  b.className = className; b.addEventListener('click', handler); return b;
}
function render() {
  const items = state.items;
  const remaining = items.filter(t => !t.completed).length;
  $('count').textContent = items.length;
  $('progress').textContent = items.length ? `${remaining} left to do` : 'Ready when you are';
  const visible = items.filter(t => filter === 'all' || (filter === 'done' ? t.completed : !t.completed));
  $('empty').hidden = visible.length > 0;
  $('empty').querySelector('h3').textContent = items.length ? 'Nothing here for now.' : 'A clear page.';
  $('empty').querySelector('p').textContent = items.length ? 'Try another filter or add a new task.' : 'Add your first task above.';
  $('items').replaceChildren();
  visible.forEach(task => {
    const row = document.createElement('li'); row.className = `task${task.completed ? ' done' : ''}`;
    if (editing === task.id) {
      row.classList.add('editing');
      const input = document.createElement('input'); input.value = task.title; input.maxLength = 120; input.setAttribute('aria-label','Edit task');
      const save = () => {
        if (!validTitle(input.value)) return;
        mutate(`/api/items/${task.id}`, 'PATCH', {title:input.value.trim(), base_revision:task.revision});
      };
      input.addEventListener('keydown', e => {if(e.key==='Enter') save(); if(e.key==='Escape'){editing=null;render();}});
      row.append(input, button('Save', save), button('Cancel', () => {editing=null;render();}));
      $('items').append(row); input.focus(); return;
    }
    const checkbox = document.createElement('input'); checkbox.type = 'checkbox'; checkbox.checked = task.completed;
    checkbox.setAttribute('aria-label', `${task.completed ? 'Reopen' : 'Complete'} ${task.title}`);
    checkbox.addEventListener('change', () => mutate(`/api/items/${task.id}`, 'PATCH', {completed:checkbox.checked, base_revision:task.revision}));
    const title = document.createElement('span'); title.className = 'task-title'; title.textContent = task.title;
    row.append(checkbox, title, button('Edit', () => {editing=task.id;render();}), button('Delete', () => {
      if (confirm(`Delete “${task.title}”?`)) mutate(`/api/items/${task.id}`, 'DELETE', {base_revision:task.revision});
    }, 'delete'));
    $('items').append(row);
  });
  renderedRevision = state.revision;
}
function validTitle(value) {
  if (!value.trim() || encoder.encode(value.trim()).length > 120) {error('Please enter a task up to 120 UTF-8 bytes (shorter for emoji).');return false;}
  return true;
}
async function refresh() {
  if (busy) return;
  try {
    state = await api('/api/state');
    const transport = state.transport;
    const published = transport.mqtt_connected && transport.published_revision >= state.revision;
    $('connection').textContent = published ? '● MQTT ready' : '● MQTT publishing…';
    $('connection').classList.toggle('offline', !transport.mqtt_connected);
    $('saved').textContent = published ? `Saved · Published revision ${state.revision}` : `Saved locally · MQTT update pending`;
    $('topic').textContent = transport.topic;
    $('broker').textContent = `${transport.broker_host}:${transport.mqtt_port}`;
    $('device-heading').textContent = transport.device_connected ? 'Tablet connected' : 'Waiting for tablet';
    $('device-status').textContent = transport.device_connected
      ? (transport.device_revision >= state.revision ? `Tablet received revision ${state.revision}. Open its To-do list to view tasks.` : 'Tablet connected; latest task list is on its way.')
      : 'No recent tablet heartbeat. Saved tasks will be sent when it reconnects.';
    if (renderedRevision !== state.revision && !editing) render();
  } catch(e) {
    $('connection').textContent = '● Server offline'; $('connection').classList.add('offline');
    $('saved').textContent = 'Cannot reach the local server';
  }
}
async function mutate(url, method, body) {
  if (busy) return false;
  busy = true; error('');
  document.querySelectorAll('button,input').forEach(el => el.disabled = true);
  let success = false;
  try {state = await api(url, method, body); editing = null; render(); success = true;}
  catch(e) {error(e.message); if(!editing && state) render();}
  finally {busy = false; document.querySelectorAll('button,input').forEach(el => el.disabled = false); await refresh();}
  return success;
}
$('add-form').addEventListener('submit', async event => {
  event.preventDefault();
  const title = $('title').value.trim();
  if (!validTitle(title)) return;
  if (!pendingCreate || pendingCreate.title !== title) pendingCreate = {title, request_id:crypto.randomUUID()};
  if (await mutate('/api/items', 'POST', pendingCreate)) {pendingCreate = null; $('title').value = ''; $('title').focus();}
});
document.querySelectorAll('[data-filter]').forEach(b => b.addEventListener('click', () => {
  filter = b.dataset.filter; editing = null;
  document.querySelectorAll('[data-filter]').forEach(el => el.setAttribute('aria-pressed', String(el===b)));
  if (state) render();
}));
refresh(); setInterval(refresh, 2000);
