'use strict';

const state = {
  snapshot: null,
  selected: null,
  filtered: [],
  player: null,
  playerReady: false,
  activeSession: [],
  friendsOpen: false,
  settingsOpen: false,
};
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
  const signedIn = Boolean(status.signed_in_email);
  $('setupPage').classList.toggle('hidden', signedIn);
  $('restorePage').classList.add('hidden');
  $('mainPage').classList.toggle('hidden', !signedIn);
  $('authConfigurationHint').classList.toggle('hidden', snap.auth_configured);
  $('setupSignIn').disabled = !snap.auth_configured;
  $('errorBanner').textContent = status.error || '';
  $('errorBanner').classList.toggle('hidden', !status.error || !signedIn);
  renderIdentity(status);
  renderFriends(snap.friends, snap.worker_configured, status);
  renderStatsAndLibrary();
  renderSettings(snap);
  renderFooter(status);
}

function renderIdentity(status) {
  const name = status.signed_in_name || status.signed_in_email || 'Not signed in';
  const picture = status.signed_in_picture || '/vodlink.png';
  $('selfName').textContent = name;
  $('selfName').title = status.signed_in_email || '';
  $('selfAvatar').src = picture;
  $('accountAvatar').src = picture;
}

function renderFriends(friends, workerConfigured, status) {
  $('friendsHeader').textContent = `FRIENDS • ${friends.length}`;
  $('workerHint').classList.toggle('hidden', workerConfigured);
  $('shareVods').checked = status.share_vods;
  $('friendsPanel').classList.toggle('hidden', !state.friendsOpen);
  const root = $('friendList');
  root.replaceChildren();
  for (const friend of friends) {
    const row = document.createElement('div');
    row.className = 'friend-row';
    const avatar = document.createElement('img');
    avatar.className = 'avatar avatar-40';
    avatar.referrerPolicy = 'no-referrer';
    avatar.src = friend.picture_url || '/vodlink.png';
    const text = document.createElement('div');
    text.className = 'friend-text';
    const strong = document.createElement('strong');
    strong.textContent = friend.display_name || friend.email;
    const small = document.createElement('small');
    small.textContent = friend.display_name ? friend.email : '';
    text.append(strong, small);
    const remove = document.createElement('button');
    remove.className = 'friend-remove';
    remove.textContent = '×';
    remove.addEventListener('click', async () => {
      try { await api(`/api/friends/${encodeURIComponent(friend.email)}`, { method: 'DELETE' }); await refresh(); }
      catch (error) { toast(error.message, true); }
    });
    row.append(avatar, text, remove);
    root.append(row);
  }
}

function isMine(vod) { return !String(vod.owner_email || '').trim(); }
function vodTitle(vod) { return String(vod.title || '').trim() || (String(vod.game || '').trim() ? `${vod.game} VOD` : 'Untitled VOD'); }
function ownerText(vod) { return vod.owner_name || vod.owner_email || vod.account_email || 'Local VOD'; }
function vodEnd(vod) { return new Date(vod.started_at).getTime() + (vod.duration_ms > 0 ? vod.duration_ms : 6 * 60 * 60 * 1000); }
function vodsOverlap(a, b) {
  const as = new Date(a.started_at).getTime(), bs = new Date(b.started_at).getTime();
  if (!Number.isFinite(as) || !Number.isFinite(bs)) return a.youtube_id === b.youtube_id;
  return as <= vodEnd(b) && bs <= vodEnd(a);
}
function clampOffset(vod, seconds) {
  const value = Math.max(0, Number(seconds) || 0);
  return vod.duration_ms > 0 ? Math.min(value, vod.duration_ms / 1000) : value;
}

function renderStatsAndLibrary() {
  const snap = state.snapshot;
  $('vodCount').textContent = String(snap.vods.length);
  const ownMs = snap.vods.filter(isMine).reduce((sum, vod) => sum + Math.max(0, vod.duration_ms || 0), 0);
  $('gameTime').textContent = `${Math.floor(ownMs / 3600000)}h ${Math.floor(ownMs / 60000) % 60}m`;

  const gameFilter = $('gameFilter');
  const previous = gameFilter.value;
  gameFilter.replaceChildren(new Option('Game: All', ''));
  for (const game of snap.games) gameFilter.append(new Option(game, game));
  gameFilter.value = snap.games.includes(previous) ? previous : '';

  let items = [...snap.vods];
  const game = gameFilter.value;
  const search = $('searchVods').value.trim().toLowerCase();
  const visibility = $('visibilityFilter').value;
  if (game) items = items.filter(v => v.game === game);
  if (visibility === 'mine') items = items.filter(isMine);
  if (visibility === 'friends') items = items.filter(v => !isMine(v));
  if (search) items = items.filter(v => `${vodTitle(v)} ${v.game} ${ownerText(v)} ${v.youtube_id}`.toLowerCase().includes(search));
  const sort = $('sortFilter').value;
  items.sort((a, b) => {
    if (sort === 'game') return String(a.game).localeCompare(String(b.game), undefined, { sensitivity: 'base' });
    if (sort === 'duration') return (a.duration_ms || 0) - (b.duration_ms || 0);
    if (sort === 'title') return vodTitle(a).localeCompare(vodTitle(b), undefined, { sensitivity: 'base' });
    return new Date(a.started_at) - new Date(b.started_at);
  });
  if ($('orderFilter').value === 'newest') items.reverse();

  const grouped = [];
  for (const vod of items) {
    const match = grouped.find(rep => String(rep.game).localeCompare(String(vod.game), undefined, { sensitivity: 'base' }) === 0 && vodsOverlap(rep, vod));
    if (match) {
      if (!isMine(match) && isMine(vod)) Object.assign(match, vod);
    } else grouped.push({ ...vod });
  }
  state.filtered = grouped;
  renderVodGrid();
}

function renderVodGrid() {
  const root = $('vodGrid');
  root.replaceChildren();
  if (!state.filtered.length) {
    const empty = document.createElement('div');
    empty.className = 'empty-library';
    empty.textContent = 'No VODs yet. Start a supported game and VodLink will create a YouTube VOD automatically.';
    root.append(empty);
    return;
  }
  for (const vod of state.filtered) {
    const card = document.createElement('article');
    card.className = `vod-card ${state.selected?.youtube_id === vod.youtube_id ? 'selected' : ''}`;
    const image = document.createElement('img');
    image.className = 'thumb';
    image.loading = 'lazy';
    image.referrerPolicy = 'no-referrer';
    image.src = `https://i.ytimg.com/vi/${encodeURIComponent(vod.youtube_id)}/hqdefault.jpg`;
    const info = document.createElement('div'); info.className = 'vod-info';
    const title = document.createElement('div'); title.className = 'vod-title'; title.textContent = vodTitle(vod);
    const meta = document.createElement('div'); meta.className = 'vod-meta';
    const left = document.createElement('span'); left.textContent = `${relativeTime(vod.started_at)} · ${formatDuration(vod.duration_ms)}`;
    const right = document.createElement('span'); right.textContent = ownerText(vod);
    meta.append(left, right); info.append(title, meta); card.append(image, info);
    card.addEventListener('click', () => selectVod(vod));
    root.append(card);
  }
}

function selectVod(vod, offset = 0) {
  state.selected = vod;
  state.activeSession = state.snapshot.vods
    .filter(candidate => candidate.youtube_id !== vod.youtube_id && String(candidate.game).localeCompare(String(vod.game), undefined, { sensitivity: 'base' }) === 0 && vodsOverlap(vod, candidate));
  state.activeSession.unshift(vod);
  $('viewerPanel').classList.remove('hidden');
  loadPlayer(vod, offset);
  renderParticipants();
  loadClips(vod.youtube_id);
  renderVodGrid();
}

function clearViewer() {
  state.selected = null;
  state.activeSession = [];
  $('viewerPanel').classList.add('hidden');
  $('youtubePlayer').classList.add('hidden');
  $('playerPlaceholder').classList.remove('hidden');
  if (state.player?.stopVideo) state.player.stopVideo();
  renderVodGrid();
}

function loadPlayer(vod, seconds) {
  const offset = clampOffset(vod, seconds);
  state.selected = vod;
  $('viewerTitle').textContent = vodTitle(vod);
  $('viewerMeta').textContent = `${ownerText(vod)} — ${vod.game} · ${new Date(vod.started_at).toLocaleString()} · ${formatDuration(vod.duration_ms)}`;
  $('deleteVodButton').textContent = isMine(vod) ? 'Delete' : 'Remove';
  $('playerPlaceholder').classList.add('hidden');
  $('youtubePlayer').classList.remove('hidden');
  if (state.playerReady && state.player?.loadVideoById) state.player.loadVideoById({ videoId: vod.youtube_id, startSeconds: Math.floor(offset) });
  else if (window.YT?.Player) createPlayer(vod, offset);
}

function createPlayer(vod, seconds = 0) {
  state.player = new YT.Player('youtubePlayer', {
    videoId: vod.youtube_id,
    playerVars: { autoplay: 1, start: Math.floor(seconds), rel: 0 },
    events: { onReady: () => { state.playerReady = true; } }
  });
}
window.onYouTubeIframeAPIReady = () => { if (state.selected) createPlayer(state.selected, 0); };

function currentPlayerOffset() { return state.playerReady && state.player?.getCurrentTime ? state.player.getCurrentTime() : 0; }
function switchParticipant(target) {
  if (!state.selected) return;
  const absolute = new Date(state.selected.started_at).getTime() + currentPlayerOffset() * 1000;
  const targetOffset = clampOffset(target, (absolute - new Date(target.started_at).getTime()) / 1000);
  loadPlayer(target, targetOffset);
  renderParticipants();
  loadClips(target.youtube_id);
}
function renderParticipants() {
  const root = $('participantStrip'); root.replaceChildren();
  for (const vod of state.activeSession) {
    const button = document.createElement('button'); button.className = `participant ${vod.youtube_id === state.selected?.youtube_id ? 'selected' : ''}`;
    const img = document.createElement('img'); img.referrerPolicy = 'no-referrer'; img.src = vod.owner_picture_url || '/vodlink.png';
    button.append(img); button.title = `Open ${ownerText(vod)}'s VOD`; button.addEventListener('click', () => switchParticipant(vod)); root.append(button);
  }
}

async function loadClips(youtubeId) {
  const root = $('clipList'); root.replaceChildren();
  try {
    const clips = await api(`/api/clips/${encodeURIComponent(youtubeId)}`);
    for (const clip of clips) {
      const row = document.createElement('div'); row.className = 'clip-row';
      const label = document.createElement('span'); label.textContent = `${clip.title} · ${formatSeconds(clip.start_seconds)}–${formatSeconds(clip.end_seconds)}`;
      const play = document.createElement('button'); play.className = 'ghost'; play.textContent = 'Play'; play.addEventListener('click', () => loadPlayer(state.selected, clip.start_seconds));
      row.append(label, play); root.append(row);
    }
  } catch (error) { toast(error.message, true); }
}

function renderFooter(status) {
  const game = String(status.last_game || '').trim();
  const who = status.signed_in_name || status.signed_in_email || '';
  $('footerIdentity').textContent = game ? `Last game: ${game}` : (who ? `Signed in as ${who}` : '');
  const button = $('autoRecordFooter');
  button.className = 'footer-auto';
  if (status.current_game) {
    button.disabled = true;
    if (status.streaming) { button.textContent = '●  STREAMING'; button.classList.add('live'); }
    else { button.textContent = '●  WAITING'; button.classList.add('waiting'); }
  } else {
    button.disabled = false;
    button.textContent = `●  Auto-recording: ${status.auto_record ? 'ON' : 'OFF'}`;
    button.classList.add(status.auto_record ? 'on' : 'off');
  }
}

function renderSettings(snap) {
  const status = snap.status;
  $('settingsOverlay').classList.toggle('hidden', !state.settingsOpen);
  $('encoder').value = snap.recorder.encoder;
  $('bitrate').value = String(snap.recorder.bitrate_kbps);
  $('resolution').value = `${snap.recorder.width}x${snap.recorder.height}`;
  $('fps').value = String(snap.recorder.fps);
  $('privacyMode').value = status.privacy_mode;
  $('microphone').checked = status.microphone;
  $('stopButton').disabled = !status.current_game;
  $('syncButton').disabled = !status.signed_in_email;
  $('settingsSignInButton').classList.toggle('hidden', Boolean(status.signed_in_email));
  $('accountDetails').textContent = status.signed_in_email ? `${status.signed_in_name || status.signed_in_email}\n${status.signed_in_email}` : 'No Google account connected.';
}
window.vodlinkOpenSettings = () => { state.settingsOpen = true; renderSettings(state.snapshot); };

async function updateSetting(body, message = '') {
  try { await api('/api/settings', { method: 'POST', body: JSON.stringify(body) }); if (message) toast(message); await refresh(); }
  catch (error) { toast(error.message, true); }
}

function qualityTier(resolution) {
  const [w, h] = String(resolution).toLowerCase().split('x').map(Number); const pixels = (w || 1920) * (h || 1080);
  if (pixels <= 640 * 360) return 360; if (pixels <= 854 * 480) return 480; if (pixels <= 1280 * 720) return 720; if (pixels <= 1920 * 1080) return 1080; if (pixels <= 2560 * 1440) return 1440; return 2160;
}
function recommendedBitrate() {
  const tier = qualityTier($('resolution').value), high = Number($('fps').value) >= 50, efficient = /av1|hevc|265/i.test($('encoder').value);
  let h264 = 12000, min = 4000, max = 10000;
  if (tier >= 2160) { h264 = high ? 35000 : 30000; min = high ? 10000 : 8000; max = high ? 40000 : 35000; }
  else if (tier >= 1440) { h264 = high ? 24000 : 15000; min = high ? 6000 : 5000; max = high ? 30000 : 25000; }
  else if (tier >= 1080) { h264 = high ? 12000 : 10000; min = high ? 4000 : 3000; max = high ? 10000 : 8000; }
  else if (tier >= 720) { h264 = high ? 6000 : 4000; min = 3000; max = 8000; }
  else { h264 = 4000; min = 3000; max = 8000; }
  return efficient ? Math.max(min, Math.min(max, h264)) : h264;
}
async function saveQualityField(field) {
  const body = {};
  if (field === 'encoder') body.encoder = $('encoder').value;
  if (field === 'resolution') body.resolution = $('resolution').value;
  if (field === 'fps') body.fps = Number($('fps').value);
  body.bitrate_kbps = recommendedBitrate();
  $('bitrate').value = String(body.bitrate_kbps);
  await updateSetting(body);
}

function formatDuration(ms) { if (!ms) return '0:00'; const t=Math.max(0,Math.floor(ms/1000)),h=Math.floor(t/3600),m=Math.floor(t%3600/60),s=t%60; return h?`${h}:${String(m).padStart(2,'0')}:${String(s).padStart(2,'0')}`:`${m}:${String(s).padStart(2,'0')}`; }
function formatSeconds(s){const t=Math.max(0,Math.floor(s||0)),m=Math.floor(t/60);return `${m}:${String(t%60).padStart(2,'0')}`;}
function relativeTime(when){const d=new Date(when),sec=Math.floor((Date.now()-d.getTime())/1000);if(sec<120)return'just now';if(sec<3600)return`${Math.floor(sec/60)}m ago`;if(sec<86400)return`${Math.floor(sec/3600)}h ago`;return d.toLocaleDateString(undefined,{month:'short',day:'numeric',year:'numeric'});}

$('setupSignIn').addEventListener('click', async()=>{try{await api('/api/sign-in',{method:'POST'});await refresh();}catch(e){toast(e.message,true);}});
$('signInAgain').addEventListener('click', async()=>{try{await api('/api/sign-out',{method:'POST'});await refresh();}catch(e){toast(e.message,true);}});
$('friendsToggle').addEventListener('click',()=>{state.friendsOpen=!state.friendsOpen;render();});
$('closeFriends').addEventListener('click',()=>{state.friendsOpen=false;render();});
$('accountButton').addEventListener('click',()=>$('accountMenu').classList.toggle('hidden'));
$('settingsButton').addEventListener('click',()=>{ $('accountMenu').classList.add('hidden'); window.vodlinkOpenSettings(); });
$('signOutButton').addEventListener('click',async()=>{try{await api('/api/sign-out',{method:'POST'});$('accountMenu').classList.add('hidden');await refresh();}catch(e){toast(e.message,true);}});
$('addFriendButton').addEventListener('click',async()=>{try{await api('/api/friends',{method:'POST',body:JSON.stringify({email:$('friendEmail').value})});$('friendEmail').value='';await refresh();}catch(e){toast(e.message,true);}});
$('friendEmail').addEventListener('keydown',e=>{if(e.key==='Enter')$('addFriendButton').click();});
$('shareVods').addEventListener('change',()=>updateSetting({share_vods:$('shareVods').checked}));
for(const id of ['searchVods','gameFilter','sortFilter','orderFilter','visibilityFilter']) $(id).addEventListener(id==='searchVods'?'input':'change',renderStatsAndLibrary);
$('closeViewer').addEventListener('click',clearViewer);
$('openYoutubeButton').addEventListener('click',()=>{if(!state.selected)return;window.open(`https://www.youtube.com/watch?v=${encodeURIComponent(state.selected.youtube_id)}&t=${Math.floor(currentPlayerOffset())}s`,'_blank');});
$('deleteVodButton').addEventListener('click',async()=>{if(!state.selected)return;const vod=state.selected;const mine=isMine(vod);const question=mine?`Delete "${vodTitle(vod)}" from YouTube and remove it from VodLink?\n\nThis cannot be undone from VodLink.`:`Remove ${ownerText(vod)}'s linked VOD from this device? This does not delete anything from YouTube.`;if(!confirm(question))return;try{if(mine)await api(`/api/vods/${encodeURIComponent(vod.youtube_id)}`,{method:'DELETE'});else await api(`/api/friend-vods/${encodeURIComponent(vod.youtube_id)}`,{method:'DELETE'});clearViewer();await refresh();}catch(e){toast(e.message,true);}});
$('importClipButton').addEventListener('click',async()=>{if(!state.selected)return toast('Select a VOD first.',true);try{await api('/api/clips/import',{method:'POST',body:JSON.stringify({youtube_id:state.selected.youtube_id,url:$('clipUrl').value})});$('clipUrl').value='';await loadClips(state.selected.youtube_id);}catch(e){toast(e.message,true);}});
$('autoRecordFooter').addEventListener('click',()=>{if(!state.snapshot)return;updateSetting({auto_record:!state.snapshot.status.auto_record});});
for(const id of ['closeSettings','closeSettingsTop'])$(id).addEventListener('click',()=>{state.settingsOpen=false;renderSettings(state.snapshot);});
$('encoder').addEventListener('change',()=>saveQualityField('encoder'));
$('resolution').addEventListener('change',()=>saveQualityField('resolution'));
$('fps').addEventListener('change',()=>saveQualityField('fps'));
$('bitrate').addEventListener('change',()=>updateSetting({bitrate_kbps:Number($('bitrate').value)}));
$('privacyMode').addEventListener('change',()=>updateSetting({privacy_mode:$('privacyMode').value}));
$('microphone').addEventListener('change',()=>updateSetting({microphone:$('microphone').checked}));
$('syncButton').addEventListener('click',async()=>{try{await api('/api/sync',{method:'POST'});await refresh();}catch(e){toast(e.message,true);}});
$('stopButton').addEventListener('click',async()=>{try{await api('/api/record/stop',{method:'POST'});await refresh();}catch(e){toast(e.message,true);}});
$('addGameButton').addEventListener('click',()=>$('manualGameForm').classList.toggle('hidden'));
$('confirmGameButton').addEventListener('click',async()=>{try{await api('/api/games',{method:'POST',body:JSON.stringify({executable:$('gameExecutable').value,name:$('gameName').value})});$('manualGameForm').classList.add('hidden');$('gameExecutable').value='';$('gameName').value='';toast('Game added');}catch(e){toast(e.message,true);}});
$('settingsSignInButton').addEventListener('click',async()=>{try{await api('/api/sign-in',{method:'POST'});await refresh();}catch(e){toast(e.message,true);}});
$('resetButton').addEventListener('click',()=>toast('Reset parity is not wired yet.',true));

refresh();
setInterval(refresh,3000);
