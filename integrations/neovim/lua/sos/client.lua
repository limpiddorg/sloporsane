local Transport = require('sos.transport')
local M = {}

function M.new(options)
  local self = setmetatable({
    transport = Transport.new(options),
    conversation = { session_id = nil, draft = '', history = nil },
    uncertain = {}, operations = {}, synchronized = false,
  }, { __index = M })
  self.transport.on_event = function(event) self:handle(event) end
  return self
end

function M:handle(event)
  if event.type == 'connection.closed' then
    self.synchronized = false
    self.recovering = false
    self.transfer = nil
    for id, operation in pairs(self.operations) do
      self.uncertain[id] = operation
    end
    self.operations = {}
  elseif event.type == 'daemon.ready' and self.recovering and not self.defer_recovery then
    self:recover_session()
  elseif event.type == 'session.attach_started' and self.recovering then
    self.transfer = { header = event, chunks = {} }
  elseif event.type == 'session.history_chunk' and self.transfer then
    if event.requestId == self.transfer.header.requestId then
      table.insert(self.transfer.chunks, event)
    end
  elseif event.type == 'session.attached' and self.transfer then
    local header = self.transfer.header
    local requested = self.conversation.session_id
    if event.requestId ~= header.requestId or event.sessionId ~= header.sessionId
      or (event.sessionId ~= requested and (header.session or {}).discussion_id ~= requested) then
      self.transport:stop('Unexpected session recovery transfer')
      return
    end
    self.conversation.session_id = event.sessionId
    self.conversation.history = self.transfer
    self.transfer = nil
    self.synchronized = true
    self.recovering = false
    -- Refresh workflow state, never replay the interrupted operation.
    self:request({ type = 'implementation.state', sessionId = event.sessionId }, 'implementation.updated')
  elseif event.type == 'request.failed' and self.recovering then
    self.recovering = false
    self.transfer = nil
  end
  if self.on_event then self.on_event(event) end
end

function M:request(frame, terminal, callback)
  local id, err = self.transport:request(frame, terminal, function(response, failure, lost_id)
    -- On disconnect, keep the operation until connection.closed archives it.
    if response then self.operations[response.requestId] = nil end
    if callback then callback(response, failure, lost_id) end
  end)
  if id then self.operations[id] = vim.deepcopy(frame) end
  return id, err
end

function M:recover_session()
  local session = self.conversation.session_id
  if session then
    self:request({type='session.attach',sessionId=session}, 'session.attached')
  else
    self.recovering = false
    self.synchronized = true
  end
end

function M:connect()
  self.transport:connect()
end

-- Reconnect is intentionally user initiated. Conversation data and uncertain
-- operations survive; only readback/reattachment requests are sent on recovery.
function M:reconnect()
  if self.transport.state ~= 'disconnected' then return end
  self.recovering = true
  self:connect()
end

function M:close()
  self.transport:close()
end

return M
