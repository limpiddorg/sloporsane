local uv = vim.uv
local Transport = require('sos.transport')
local Client = require('sos.client')
local dir = vim.fn.getcwd() .. '/build-ai/neovim/' .. string.rep('long-', 20)
vim.fn.mkdir(dir, 'p')
dir = assert(uv.fs_mkdtemp(dir .. '/run-XXXXXX'))
local sockets_dir = assert(vim.env.SOS_TEST_SOCKET_DIR)
local count = 0
local function wait(predicate)
  assert(vim.wait(4000, predicate, 5), 'Timed out waiting for test condition')
end
local function encode(value) return vim.json.encode(value) .. '\n' end
local function peer(hello, responder)
  count = count + 1
  -- Unix socket paths have small platform-specific length limits.
  local path = sockets_dir .. '/p' .. count
  uv.fs_unlink(path)
  local server = uv.new_pipe(false)
  assert(server:bind(path))
  local connections, received = {}, {}
  server:listen(8, function()
    local socket = uv.new_pipe(false)
    server:accept(socket)
    table.insert(connections, socket)
    local buffer = ''
    socket:read_start(function(_, data)
      if not data then return end
      buffer = buffer .. data
      while buffer:find('\n', 1, true) do
        local at = buffer:find('\n', 1, true)
        local frame = vim.json.decode(buffer:sub(1, at - 1))
        buffer = buffer:sub(at + 1)
        table.insert(received, frame)
        if responder then responder(socket, frame) end
      end
    end)
    if hello ~= false then socket:write(hello or encode({ type = 'daemon.ready', protocolVersion = 7 })) end
  end)
  return path, received, connections, function()
    for _, socket in ipairs(connections) do if not socket:is_closing() then socket:close() end end
    server:close()
    uv.fs_unlink(path)
  end
end
local function options(path)
  return { socket = path, auto_start = false, timeout_ms = 500 }
end

-- Loading must not open connections or change user mappings/cwd.
local cwd = vim.fn.getcwd()
vim.keymap.set('n', '<F8>', ':echo "user"<CR>')
local mapping = vim.fn.maparg('<F8>', 'n')
dofile('integrations/neovim/plugin/sos.lua')
require('sos').setup({ auto_start = false })
assert(vim.fn.getcwd() == cwd and vim.fn.maparg('<F8>', 'n') == mapping)
assert(package.loaded['sos.client'] and vim.fn.exists(':SosReconnect') == 2)
assert(require('sos').client().transport.state == 'disconnected')

for _, address in ipairs({'/' .. string.rep('x', 108), '/' .. string.rep('é', 54)}) do
  local invalid = Transport.new({socket=address, auto_start=true})
  local reason
  invalid.on_event = function(event) reason = event.error end
  invalid:connect()
  assert(reason and reason:find('shorter absolute socket path', 1, true))
  assert(invalid.state == 'disconnected' and not invalid.pipe and not invalid.launcher)
end

local path, received, sockets, cleanup = peer(false)
local t = Transport.new(options(path))
t:connect()
wait(function() return #sockets == 1 end)
sockets[1]:write('{"type":"daemon.')
sockets[1]:write('ready","protocolVersion":7}\n' .. encode({type='integration.ping', pingId='p'}))
wait(function() return #received == 1 end)
assert(t.state == 'ready' and received[1].type == 'integration.pong')
local replies = 0
local id = assert(t:request({type='probe'}, 'probe.done', function(frame)
  assert(frame.type == 'probe.done'); replies = replies + 1
end))
sockets[1]:write(encode({type='probe.accepted',requestId=id}) .. encode({type='probe.done',requestId=id}))
wait(function() return replies == 1 end)
assert(not t.pending[id])
local oversized, error = t:request({type='probe',text=string.rep('x',16*1024*1024)}, 'probe.done')
assert(not oversized and error:find('16 MiB'))
t:close()
wait(function() return received[#received].type == 'integration.close' end)
cleanup()

for _, hello in ipairs({encode({type='daemon.ready',protocolVersion=6}), '[]\n', 'invalid\n'}) do
  local p, _, _, done = peer(hello)
  local broken = Transport.new(options(p))
  local reason
  broken.on_event = function(event) reason = event.error end
  broken:connect()
  wait(function() return reason ~= nil end)
  assert(broken.state == 'disconnected')
  done()
end
local p, _, _, done = peer(false)
local timeout = Transport.new(options(p))
timeout:connect()
wait(function() return timeout.state == 'disconnected' end)
done()

-- Loss keeps the conversation and records uncertain requests, without replay.
local p2, frames, clients, done2 = peer(nil, function(socket, frame)
  if frame.type == 'session.attach' then
    socket:write(encode({type='session.attach_started', requestId=frame.requestId,
      sessionId='saved', session={id='saved'}})
      .. encode({type='session.history_chunk',requestId=frame.requestId,sessionId='saved',messages={}})
      .. encode({type='session.attached',requestId=frame.requestId,sessionId='saved'}))
  elseif frame.type == 'implementation.state' then
    socket:write(encode({type='implementation.updated',requestId=frame.requestId,implementation=vim.NIL}))
  end
end)
local client = Client.new(options(p2))
client.conversation.session_id = 'saved'
client.conversation.draft = 'unsent'
client:connect()
wait(function() return client.transport.state == 'ready' end)
local lost = assert(client:request({type='message.send',text='one'}, 'message.accepted'))
wait(function() return #frames == 1 end)
clients[1]:close()
wait(function() return client.transport.state == 'disconnected' end)
assert(client.uncertain[lost].text == 'one' and client.conversation.draft == 'unsent')
local before = #clients
vim.wait(80, function() return false end, 5)
assert(#clients == before)
client:reconnect()
wait(function() return client.synchronized and #frames >= 3 end)
assert(frames[2].type == 'session.attach' and frames[3].type == 'implementation.state')
assert(#client.conversation.history.chunks == 1 and client.conversation.draft == 'unsent')
client:close()
done2()

-- Oversized unterminated inbound frames are rejected before JSON decoding.
local bigpath, _, bigclients, bigdone = peer()
local big = Transport.new(options(bigpath))
big:connect()
wait(function() return big.state == 'ready' end)
bigclients[1]:write(string.rep('x', 16 * 1024 * 1024))
wait(function() return big.state == 'disconnected' end)
bigdone()

-- Startup checks use Python's standard library and a controlled daemon executable.
local result = vim.fn.system({'python3', 'integrations/neovim/tests/startup.py', dir})
assert(vim.v.shell_error == 0, result)
print(result)
local state = vim.env.XDG_STATE_HOME
vim.env.XDG_STATE_HOME = dir
local launch = Transport.new({socket=sockets_dir .. '/a', daemon=dir .. '/fake-sosd',
  auto_start=true, python='python3', timeout_ms=3000})
local failure
launch.on_event = function(event) failure = event.error end
local connected, launch_error = xpcall(function()
  launch:connect()
  wait(function() return launch.state == 'ready' or failure ~= nil end)
  assert(launch.state == 'ready', failure)
end, debug.traceback)
launch:close()
vim.env.XDG_STATE_HOME = state
local drained = vim.wait(7000, function() return not uv.fs_stat(sockets_dir .. '/a') end, 20)
assert(connected, launch_error)
assert(drained, 'Controlled startup daemon did not drain')
