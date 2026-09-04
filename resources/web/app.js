'use strict';

const state = { snapshot: null, selected: null, player: null, playerReady: false, activeSession: [] };
const $ = id => document.getElementById(id);

async function api(path, options = {}) {
  const request = { ...options, headers: { 'Content-Type': 'application/json', ...(options.headers || {}) } };
  const response = await fetch(path, request);
  const text = await response.text();
  let body = null;
  try { body = text ? JSON.parse(text) : null; } catch { body = { message: text }; }
  if (!response.ok) throw new Error(body?.message || `Request failed: ${response.status}`);
  return body;
}

function toast(message, isError = false) {
  const node = $('toast');
  node.textContent = message;
  node.style.borderColor = isError ? '#8c3a4d' : '#405576';
  node.classList.remove('hidden');
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => node.classList.add('hidden'), 3500);
}

async function refresh() {
  try {
    state.snapshot = await api('/api/snapshot');
    render();
  } catch (error) {
    $('errorBanner').textContent = error.message;
    $('errorBanner').classList.remove('hidden');
  }
}

function render() {
  const snap = state.snapshot;
  if (!snap) return;
  const status = snap.status;
  $('statusMessage').textContent = status.message || 'Watching for games';
  $('statusDot').className = `status-dot ${status.streaming ? 'live' : 'ready'}`;
  $('stopButton').classList.toggle('hidden', !status.streaming && !status.current_game);
  $('errorBanner').textContent = status.error || '';
  $('errorBanner').classList.toggle('hidden', !status.error);

  renderAccount(status);
  renderLibrary(snap.vods, snap.games);
  renderFriends(snap.friends);
  renderSettings(snap);
}

function renderAccount(status) {
  const account = $('account');
  account.replaceChildren();
  if (!status.signed_in_email) {
    account.textContent = 'Not signed in';
    return;
  }
  if (status.signed_in_picture) {
    const image = document.createElement('img');
    image.className = 'avatar';
    image.referrerPolicy = 'no-referrer';
    image.src = status.signed_in_picture;
    account.append(image);
  }
  const text = document.createElement('span');
  text.textContent = status.signed_in_name || status.signed_in_email;
  account.append(text);
}

function renderLibrary(vods, games) {
  const filter = $('gameFilter');
  const previous = filter.value;
  filter.replaceChildren(new Option('All games', ''));
  for (const game of games) filter.append(new Option(game, game));
  filter.value = games.includes(previous) ? previous : '';

  const visible = filter.value ? vods.filter(vod => vod.game === filter.value) : vods;
  const list = $('vodList');
  list.replaceChildren();
  if (!visible.length) {
    const empty = document.createElement('div');
    empty.className = 'player-placeholder';
    empty.textContent = 'No VODs yet.';
    list.append(empty);
    return;
  }
  for (const vod of visible) {
    const card = document.createElement('div');
    card.className = `vod-card ${state.selected?.youtube_id === vod.youtube_id ? 'selected' : ''}`;
    const title = document.createElement('h3');
    title.textContent = vod.title || vod.game || 'VOD';
    const meta = document.createElement('div');
    meta.className = 'meta';
    const owner = document.createElement('span');
    owner.className = 'owner';
    owner.textContent = vod.owner_name || vod.owner_email || 'You';
    const date = document.createElement('span');
    date.textContent = new Date(vod.started_at).toLocaleString();
    meta.append(owner, date);
    card.append(title, meta);
    card.addEventListener('click', () => selectVod(vod));
    list.append(card);
  }
}

function selectVod(vod) {
  state.selected = vod;
  const sameGame = state.snapshot.vods
    .filter(candidate => candidate.game === vod.game)
    .sort((a, b) => new Date(a.started_at) - new Date(b.started_at));
  state.activeSession = sameGame;
  renderSessionSwitcher();
  loadPlayer(vod, 0);
  loadClips(vod.youtube_id);
  renderLibrary(state.snapshot.vods, state.snapshot.games);
}

function renderSessionSwitcher() {
  const root = $('sessionSwitcher');
  root.replaceChildren();
  for (const vod of state.activeSession) {
    const button = document.createElement('button');
    button.className = vod.youtube_id === state.selected?.youtube_id ? 'active' : 'secondary';
    button.textContent = vod.owner_name || vod.owner_email || 'You';
    button.addEventListener('click', () => switchSessionVod(vod));
    root.append(button);
  }
}

function loadPlayer(vod, seconds) {
  $('playerPlaceholder').classList.add('hidden');
  $('youtubePlayer').classList.remove('hidden');
  state.selected = vod;
  if (state.playerReady && state.player?.loadVideoById) {
    state.player.loadVideoById({ videoId: vod.youtube_id, startSeconds: Math.max(0, seconds) });
  } else if (window.YT?.Player) {
    createPlayer(vod, seconds);
  }
  const owner = vod.owner_name || vod.owner_email || 'You';
  $('selectedMeta').textContent = `${vod.game} · ${owner} · ${new Date(vod.started_at).toLocaleString()} · ${formatDuration(vod.duration_ms)}`;
  renderSessionSwitcher();
}

function createPlayer(vod, seconds = 0) {
  state.player = new YT.Player('youtubePlayer', {
    videoId: vod.youtube_id,
    playerVars: { autoplay: 1, start: Math.floor(seconds), rel: 0 },
    events: { onReady: () => { state.playerReady = true; } }
  });
}

window.onYouTubeIframeAPIReady = () => {
  if (state.selected) createPlayer(state.selected, 0);
};

function switchSessionVod(target) {
  if (!state.selected) return;
  const currentSeconds = state.playerReady && state.player?.getCurrentTime ? state.player.getCurrentTime() : 0;
  const absoluteMs = new Date(state.selected.started_at).getTime() + Math.max(0, currentSeconds) * 1000;
  const relative = Math.max(0, (absoluteMs - new Date(target.started_at).getTime()) / 1000);
  state.selected = target;
  loadPlayer(target, relative);
  loadClips(target.youtube_id);
  renderLibrary(state.snapshot.vods, state.snapshot.games);
}

async function loadClips(youtubeId) {
  const root = $('clipList');
  root.replaceChildren();
  try {
    const clips = await api(`/api/clips/${encodeURIComponent(youtubeId)}`);
    for (const clip of clips) {
      const card = document.createElement('div');
      card.className = 'clip-card';
      const label = document.createElement('span');
      label.textContent = `${clip.title} · ${clip.start_seconds}s–${clip.end_seconds}s`;
      const open = document.createElement('button');
      open.className = 'secondary';
      open.textContent = 'Play';
      open.addEventListener('click', () => loadPlayer(state.selected, clip.start_seconds));
      card.append(label, open);
      root.append(card);
    }
  } catch (error) { toast(error.message, true); }
}

function renderFriends(friends) {
  const root = $('friendList');
  root.replaceChildren();
  for (const friend of friends) {
    const card = document.createElement('div');
    card.className = 'friend-card';
    const label = document.createElement('span');
    label.textContent = friend.display_name ? `${friend.display_name} · ${friend.email}` : friend.email;
    const remove = document.createElement('button');
    remove.className = 'danger';
    remove.textContent = 'Remove';
    remove.addEventListener('click', async () => {
      try { await api(`/api/friends/${encodeURIComponent(friend.email)}`, { method: 'DELETE' }); await refresh(); }
      catch (error) { toast(error.message, true); }
    });
    card.append(label, remove);
    root.append(card);
  }
}

function renderSettings(snap) {
  const status = snap.status;
  $('autoRecord').checked = status.auto_record;
  $('shareVods').checked = status.share_vods;
  $('microphone').checked = status.microphone;
  $('privacyMode').value = status.privacy_mode;
  $('encoder').value = snap.recorder.encoder;
  $('resolution').value = `${snap.recorder.width}x${snap.recorder.height}`;
  $('fps').value = String(snap.recorder.fps);
  $('bitrate').value = String(snap.recorder.bitrate_kbps);
  $('signInButton').disabled = Boolean(status.signed_in_email) || !snap.auth_configured;
  $('signOutButton').disabled = !status.signed_in_email;
  $('syncButton').disabled = !status.signed_in_email;
  $('accountDetails').textContent = status.signed_in_email
    ? `${status.signed_in_name || status.signed_in_email}\n${status.signed_in_email}`
    : 'No Google account connected.';
}

function formatDuration(ms) {
  if (!ms) return 'processing';
  const total = Math.max(0, Math.round(ms / 1000));
  const h = Math.floor(total / 3600), m = Math.floor(total % 3600 / 60), s = total % 60;
  return h ? `${h}:${String(m).padStart(2,'0')}:${String(s).padStart(2,'0')}` : `${m}:${String(s).padStart(2,'0')}`;
}

for (const button of document.querySelectorAll('.nav')) {
  button.addEventListener('click', () => {
    document.querySelectorAll('.nav').forEach(node => node.classList.toggle('active', node === button));
    document.querySelectorAll('.page').forEach(node => node.classList.add('hidden'));
    $(`page-${button.dataset.page}`).classList.remove('hidden');
  });
}

$('gameFilter').addEventListener('change', () => renderLibrary(state.snapshot.vods, state.snapshot.games));
$('stopButton').addEventListener('click', async () => { try { await api('/api/record/stop', { method: 'POST' }); await refresh(); } catch (e) { toast(e.message, true); } });
$('syncButton').addEventListener('click', async () => { try { await api('/api/sync', { method: 'POST' }); toast('Library synced'); await refresh(); } catch (e) { toast(e.message, true); } });
$('signInButton').addEventListener('click', async () => { try { toast('Complete Google sign-in in the new browser tab.'); await api('/api/sign-in', { method: 'POST' }); await refresh(); } catch (e) { toast(e.message, true); } });
$('signOutButton').addEventListener('click', async () => { try { await api('/api/sign-out', { method: 'POST' }); await refresh(); } catch (e) { toast(e.message, true); } });
$('addFriendButton').addEventListener('click', async () => { try { await api('/api/friends', { method: 'POST', body: JSON.stringify({ email: $('friendEmail').value }) }); $('friendEmail').value=''; await refresh(); } catch (e) { toast(e.message, true); } });
$('addGameButton').addEventListener('click', async () => { try { await api('/api/games', { method: 'POST', body: JSON.stringify({ executable: $('gameExecutable').value, name: $('gameName').value }) }); toast('Game added'); } catch (e) { toast(e.message, true); } });
$('saveSettingsButton').addEventListener('click', async () => {
  const body = {
    auto_record: $('autoRecord').checked,
    share_vods: $('shareVods').checked,
    microphone: $('microphone').checked,
    privacy_mode: $('privacyMode').value,
    encoder: $('encoder').value,
    bitrate_kbps: Number($('bitrate').value),
    resolution: $('resolution').value,
    fps: Number($('fps').value)
  };
  try { await api('/api/settings', { method:'POST', body: JSON.stringify(body) }); toast('Settings saved'); await refresh(); }
  catch (e) { toast(e.message, true); }
});
$('importClipButton').addEventListener('click', async () => {
  if (!state.selected) return toast('Select one of your VODs first.', true);
  try {
    await api('/api/clips/import', { method:'POST', body: JSON.stringify({ youtube_id: state.selected.youtube_id, url: $('clipUrl').value }) });
    $('clipUrl').value=''; await loadClips(state.selected.youtube_id); toast('Clip imported');
  } catch (e) { toast(e.message, true); }
});
$('dataRootButton').addEventListener('click', async () => { try { const result = await api('/api/data-root'); toast(result.message); } catch (e) { toast(e.message, true); } });
$('quitButton').addEventListener('click', async () => { try { await api('/api/shutdown', { method:'POST' }); document.body.innerHTML='<main style="padding:48px;font-family:system-ui">VodLink has stopped. You can close this tab.</main>'; } catch (e) { toast(e.message, true); } });

refresh();
setInterval(refresh, 3000);
