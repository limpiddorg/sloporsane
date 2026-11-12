local uv = vim.uv
local M = {}
local limit = 16 * 1024 * 1024
local source = debug.getinfo(1, 'S').source:sub(2)
local root = vim.fn.fnamemodify(source, ':h:h:h')

local function close(handle)
  if handle and not handle:is_closing() then handle:close() end
end

function M.new(options)
  local self = { options = options, state = 'disconnected', pending = {}, sequence = 0, epoch = 0 }
  return setmetatable(self, { __index = M })
end

function M:emit(event)
  if self.on_event then self.on_event(event) end
end

function M:stop(reason)
  self.epoch = self.epoch + 1
  close(self.pipe)
  close(self.timer)
  self.pipe, self.timer = nil, nil
  if self.launcher then
    vim.fn.chanclose(self.launcher, 'stdin')
    self.launcher = nil
  end
  self.state = 'disconnected'
  local pending = self.pending
  self.pending = {}
  for id, item in pairs(pending) do
    if item.callback then item.callback(nil, reason, id) end
  end
  self:emit({ type = 'connection.closed', error = reason })
end

function M:write(frame)
  local ok, text = pcall(vim.json.encode, frame)
  if not ok then return nil, 'Invalid JSON request' end
  text = text .. '\n'
  if #text > limit then return nil, 'Request exceeds 16 MiB frame limit' end
  if not self.pipe or self.pipe:is_closing() then return nil, 'Not connected' end
  local epoch = self.epoch
  self.pipe:write(text, function(err)
    if err then vim.schedule(function()
      if self.epoch == epoch then self:stop('Write failed: ' .. err) end
    end) end
  end)
  return true
end

-- Callers choose the terminal response type: an accepted event may precede a
-- bounded history transfer, so matching requestId alone does not imply completion.
function M:request(frame, terminal, callback)
  if self.state ~= 'ready' then return nil, 'Not connected' end
  self.sequence = self.sequence + 1
  local id = tostring(self.sequence)
  frame = vim.deepcopy(frame)
  frame.requestId = id
  self.pending[id] = { callback = callback, terminal = terminal }
  local ok, err = self:write(frame)
  if not ok then self.pending[id] = nil; return nil, err end
  return id
end

function M:receive(frame)
  if self.state == 'connecting' then
    if frame.type ~= 'daemon.ready' or frame.protocolVersion ~= 7 then
      self:stop('Unsupported daemon handshake: expected protocol 7; update daemon and plugin together')
      return
    end
    close(self.timer)
    self.timer = nil
    self.state = 'ready'
    self.info = frame.info or {}
    if self.launcher then
      vim.fn.chansend(self.launcher, 'connected\n')
      vim.fn.chanclose(self.launcher, 'stdin')
      self.launcher = nil
    end
    self:emit(frame)
    return
  end
  if frame.type == 'integration.ping' then
    if type(frame.pingId) ~= 'string' then self:stop('Invalid daemon ping'); return end
    self:write({ type = 'integration.pong', pingId = frame.pingId })
    return
  end
  local item = self.pending[frame.requestId]
  if item and (frame.type == item.terminal or frame.type == 'request.failed') then
    self.pending[frame.requestId] = nil
    if item.callback then item.callback(frame, frame.type == 'request.failed' and frame.error or nil) end
  end
  self:emit(frame)
end

function M:open_socket(on_failure)
  self.pipe = uv.new_pipe(false)
  local pipe, epoch = self.pipe, self.epoch
  local buffer = ''
  pipe:connect(self.options.socket, function(err)
    vim.schedule(function()
      if self.epoch ~= epoch then return end
      if err then close(pipe); self.pipe = nil; on_failure(err); return end
      pipe:read_start(function(read_error, data)
        vim.schedule(function()
          if self.epoch ~= epoch then return end
          if read_error or not data then self:stop(read_error or 'Daemon disconnected'); return end
          buffer = buffer .. data
          while true do
            local newline = buffer:find('\n', 1, true)
            if not newline then
              if #buffer >= limit then self:stop('Daemon frame exceeds 16 MiB limit') end
              return
            end
            if newline > limit then self:stop('Daemon frame exceeds 16 MiB limit'); return end
            local line = buffer:sub(1, newline - 1)
            buffer = buffer:sub(newline + 1)
            local ok, frame = pcall(vim.json.decode, line)
            if not ok or type(frame) ~= 'table' or type(frame.type) ~= 'string' then
              self:stop('Invalid daemon JSONL frame'); return
            end
            self:receive(frame)
            if self.epoch ~= epoch then return end
          end
        end)
      end)
    end)
  end)
end

function M:daemon_path()
  if self.options.daemon then return vim.fn.exepath(self.options.daemon) end
  local launcher = vim.fn.exepath('sos')
  if launcher == '' then launcher = vim.env.HOME .. '/.local/bin/sos' end
  local installed = vim.fn.fnamemodify(vim.fn.resolve(launcher), ':h') .. '/_internal/sos_tui/bin/sosd'
  for _, path in ipairs({ root .. '/bin/sosd', 'sosd', installed,
    root .. '/../../build-ai/bin/sosd', root .. '/../tui/src/sos_tui/bin/sosd' }) do
    local executable = vim.fn.exepath(path)
    if executable ~= '' then return executable end
  end
  return ''
end

function M:start_daemon()
  local binary = self:daemon_path()
  if binary == '' then self:stop('sosd not found: configure daemon with an executable path'); return end
  local state = vim.env.XDG_STATE_HOME
  if not state or state:sub(1, 1) ~= '/' then state = vim.env.HOME .. '/.local/state' end
  local epoch, errors = self.epoch, {}
  local stdout = ''
  local job = vim.fn.jobstart({ self.options.python or 'python3', root .. '/scripts/start_daemon.py',
    binary, self.options.socket, tostring((self.options.timeout_ms or 10000) / 1000),
    state .. '/sos/sosd.log' }, {
    stdout_buffered = false,
    on_stdout = function(_, lines)
      if self.epoch ~= epoch then return end
      stdout = stdout .. table.concat(lines, '\n')
      if stdout:find('ready\n', 1, true) then
        stdout = ''
        self:open_socket(function(err) self:stop('Connect failed: ' .. err) end)
      end
    end,
    on_stderr = function(_, lines)
      for _, line in ipairs(lines) do if line ~= '' then table.insert(errors, line) end end
    end,
    on_exit = function(_, code)
      if self.epoch == epoch and code ~= 0 then self:stop('Daemon startup failed: ' .. table.concat(errors, '\n')) end
    end,
  })
  if job <= 0 then self:stop('Cannot start daemon helper: install Python 3 or disable auto_start')
  else self.launcher = job end
end

function M:connect()
  if self.state ~= 'disconnected' then return end
  if not self.options.socket or self.options.socket:sub(1, 1) ~= '/' then
    self:emit({ type = 'connection.closed', error = 'socket must be an absolute path' }); return
  end
  local limit = uv.os_uname().sysname == 'Darwin' and 103 or 107
  if #self.options.socket > limit then
    self:emit({type = 'connection.closed', error = 'Unix socket path is too long (maximum '
      .. limit .. ' bytes); configure a shorter absolute socket path'})
    return
  end
  self.epoch = self.epoch + 1
  self.state = 'connecting'
  local epoch = self.epoch
  self.timer = uv.new_timer()
  self.timer:start(self.options.timeout_ms or 10000, 0, vim.schedule_wrap(function()
    if self.epoch == epoch then self:stop('Timed out connecting to SOS daemon') end
  end))
  self:open_socket(function(err)
    if self.options.auto_start ~= false and (err:find('ENOENT') or err:find('ECONNREFUSED')) then
      self:start_daemon()
    else self:stop('Connect failed: ' .. err) end
  end)
end

function M:close()
  if self.state == 'ready' then
    -- Queue close before shutdown; shutdown drains the pending writes.
    self:write({ type = 'integration.close' })
    local pipe = self.pipe
    self.pipe = nil
    pipe:shutdown(function() close(pipe) end)
  end
  self:stop('Connection closed by user')
end

return M
