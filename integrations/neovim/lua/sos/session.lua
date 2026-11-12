local M = {}
local History = require('sos.history')
local function fresh(workspace)
  return { draft = '', records = {}, workspace = workspace, mode = 'sane', busy = false }
end

function M.new(client, options)
  local self = setmetatable({ client = client, options = options, saved = {},
    launch_workspace = options.workspace or vim.fn.getcwd(),
    current = fresh(options.workspace or vim.fn.getcwd()), configured = false,
    waiting = {}, permissions = {}, }, { __index = M })
  client.defer_recovery = true
  self.workflow = require('sos.workflow').new(self)
  local previous = client.on_event
  client.on_event = function(event)
    if previous then previous(event) end
    self:event(event)
  end
  return self
end

function M:changed()
  self.workflow:remember(self.current)
  self.client.conversation.session_id = self.workflow.attached_id or self.current.id
  self.client.conversation.draft = self.current.draft
  if self.inspector then self.inspector:update() end
  if self.on_change then self.on_change() end
end

function M:notice(text)
  self.current.notice = text
  self:changed()
end

function M:request(frame, terminal, callback)
  local id, err = self.client:request(frame, terminal, function(response, failure)
    if not response then
      if callback then callback(nil, failure) end
      return
    end
    if response.type == 'request.failed' then
      self:notice(response.error or 'Daemon refused request')
      if callback then callback(nil, response.error) end
    elseif callback then callback(response) end
  end)
  if not id then self:notice(err) end
  return id
end

function M:run(action)
  if self.configured and self.client.transport.state == 'ready' then action(); return end
  if self.client.transport.state == 'disconnected' and self.ever_connected then
    self:notice('Disconnected. Use :SosReconnect; pending requests are not retried.')
    return
  end
  table.insert(self.waiting, action)
  self.client:connect()
end

function M:confirm_switch(action)
  if self.switching or self.current.pending then self:notice('Wait for the pending request'); return end
  local current = self.current
  if current.busy or (self.workflow.state and self.workflow.state.status=='running') or self.workflow.info.auto_plan_running then
    vim.ui.select({'Keep working', 'Cancel generation and switch'}, {prompt='Switch conversation?'}, function(choice)
      if choice == 'Cancel generation and switch' and self.current == current then action() end
    end)
  else action() end
end

function M:new_session(restored)
  self:run(function() self:confirm_switch(function()
    self.switching = true
    self:request({type='session.new'}, 'session.reset', function(event)
      self.switching = false
      if not event then return end
      if self.current.id then
        if not self.current.context_key then self.saved[self.current.id] = self.current end
      else
        self.saved['draft:' .. tostring(vim.uv.hrtime())] = self.current
      end
      self.current = fresh(event.workspace or self.current.workspace)
      self.workflow:reset()
      if restored then self.current.draft = restored.draft end
      self:request({type='mode.set',mode='sane'}, 'mode.updated')
      self:changed()
    end)
  end) end)
end

function M:attach(id)
  self:run(function() self:confirm_switch(function()
    self.switching = id
    self.attach_request = self:request({type='session.attach',sessionId=id}, 'session.attached', function(event)
      if not event then self.switching = false; self.transfer = nil; self:changed() end
    end)
  end) end)
end

function M:resume()
  self:run(function()
    self:request({type='session.list'}, 'session.listed', function(event)
      if not event then return end
      local rows = {}
      for _, session in ipairs(event.sessions or {}) do
        table.insert(rows, {id=session.id, label=session.title or session.id})
      end
      for id, session in pairs(self.saved) do
        if id:sub(1,6) == 'draft:' then table.insert(rows, {local_id=id,label='Unsent draft: ' .. session.draft:sub(1,40)}) end
      end
      vim.ui.select(rows, {prompt='SOS sessions',format_item=function(row) return row.label end}, function(row)
        if not row then return end
        if row.local_id then
          -- Detach before exposing a local draft; never send it to the prior session.
          self:new_session(self.saved[row.local_id])
        else self:attach(row.id) end
      end)
    end)
  end)
end

function M:set_draft(text)
  self.current.draft = text
  self.client.conversation.draft = text
end

function M:history(current)
  current = current or self.current
  if not current.history then current.history = History.new(self,current) end
  return current.history
end

function M:navigate_history(direction)
  if self.current.kind=='review' or self.current.context_key then self:notice('Switch to Discussion or planning to navigate history'); return end
  self:history():navigate(direction)
end
function M:older() self:navigate_history('before') end
function M:newer() self:navigate_history('after') end
function M:latest() self:navigate_history('latest') end
function M:contexts() self.workflow:choose_context() end
function M:plan() self.workflow:objectives(false) end
function M:auto_plan() self.workflow:objectives(true) end
function M:start() self.workflow:start() end
function M:retry_plan() self.workflow:auto_retry() end
function M:implementation_plan() self.workflow:show_plan() end
function M:generate_plan() self.workflow:generate() end
function M:steps() self.workflow:steps() end
function M:continue() self.workflow:resume() end
function M:inspect()
  if self.current.kind~='step' and self.workflow.selected_identity then self.workflow:select_step(self.workflow.selected_identity) end
  if not self.inspector then self.inspector=require('sos.inspection').new(self) end
  self.inspector:show()
end
function M:inspection_action(action)
  if not self.inspector or not self.inspector.visible then self:inspect() end
  if self.inspector and self.inspector.visible then self.inspector[action](self.inspector) end
end
function M:next_substep() self:inspection_action('next_substep') end
function M:previous_substep() self:inspection_action('previous_substep') end
function M:next_change() self:inspection_action('next_change') end
function M:previous_change() self:inspection_action('previous_change') end
function M:explanation() self:inspection_action('toggle_explanation') end
function M:review_actions()
  if not self.workflow.review_actions then self.workflow.review_actions=require('sos.review').new(self.workflow) end
  return self.workflow.review_actions
end
function M:indicator() self:review_actions():indicator() end
function M:validate() self:review_actions():validate() end
function M:proposal() self:review_actions():proposal() end
function M:accept_proposal() self:review_actions():proposal_action(true) end
function M:reject_proposal() self:review_actions():proposal_action(false) end
function M:revision_plan() self:review_actions():revision() end
function M:versions() self:review_actions():versions() end
function M:review_question()
  local step,part=self:review_actions():selection()
  if step then self.workflow:review(step,part) end
end

function M:send()
  local current = self.current
  if current.kind=='review' or current.kind=='revision' then self.workflow:send_review(current); return end
  if self.workflow:readonly(current) then self:notice('This context is read-only; finish implementation or switch context'); return end
  if current.busy or current.pending or self.switching then self:notice('Wait for the current operation'); return end
  if current.draft:match('^%s*$') then return end
  if current.kind~='planning' and self.info and (self.info.implementation_view or (type(self.info.active_implementation_id) == 'string'
    and self.info.active_implementation_id ~= '')) then
    self:notice('Implementation conversations are not writable from this Discussion input'); return
  end
  self:run(function()
    if self.current ~= current or current.pending or current.busy or self.switching then return end
    local history = self:history()
    if history.loading then self:notice('Wait for history to finish loading'); return end
    if history.page.hasNewer then
      history:navigate('latest',function(ok) if ok and self.current == current then self:send() end end)
      return
    end
    history:purge()
    current.permission_decision = nil
    current.notice = nil
    local text = current.draft
    local frame = {type='message.send', text=text}
    if not current.id then frame.workspace = current.workspace end
    current.pending = {text=text}
    local id = self:request(frame, current.id and 'message.accepted' or 'session.created', function(event)
      current.pending = nil
      if event then
        if event.session then
          current.id = event.session.id
          self.saved[current.id] = current
        end
        current.busy = true
        if current.draft == text then current.draft = '' end
      end
      self:changed()
    end)
    if not id then current.pending = nil end
    self:changed()
  end)
end

function M:toggle_mode()
  if self.current.kind~='discussion' or self.workflow:readonly(self.current) then
    self:notice('Mode changes are available only in editable Discussion'); return
  end
  self:run(function()
    self:request({type='mode.set',mode=self.current.mode == 'sane' and 'slop' or 'sane'}, 'mode.updated')
  end)
end

function M:cancel()
  if self.workflow.archive_view or self.workflow.info.implementation_archive then self:notice('Archived implementation is read-only'); return end
  self:run(function()
    if self.current.kind=='review' or self.current.kind=='revision' then
      self:request({type='implementation.review.cancel',sessionId=self.workflow.branch_id},'implementation.updated')
      return
    end
    if self.workflow.state and self.workflow.state.status=='running' then self.workflow:cancel_construction(); return end
    -- Either acknowledgement is nonterminal for the actual generation.
    self:request({type='execution.cancel'}, 'execution.cancel_requested')
  end)
end

local function accept_transfer(self, event)
  local transfer = self.transfer
  if not transfer or event.requestId ~= transfer.header.requestId then return end
  local records = History.accept(transfer,event)
  if records then
    local metadata = transfer.header.session or {}
    local requested = self.switching or self.current.id
    assert(event.sessionId == requested or metadata.discussion_id == requested, 'Unexpected attachment')
    if self.current.id then
      if not self.current.context_key then self.saved[self.current.id] = self.current end
    else self.saved['draft:' .. tostring(vim.uv.hrtime())] = self.current end
    local current = self.saved[event.sessionId] or fresh(metadata.workspace or self.current.workspace)
    self:history():invalidate()
    current.id, current.busy = event.sessionId, false
    self:history(current):install(records,transfer.header.page)
    current.mode = metadata.mode or current.mode
    self.current, self.transfer, self.switching = current, nil, false
    self.saved[current.id] = current
    self.workflow:attached(transfer.header)
  end
end

function M:event(event)
  if event.info then
    self.info = event.info
    if event.info.workspace and not self.current.preview then self.current.workspace = event.info.workspace end
    if event.info.mode then self.current.mode = event.info.mode end
  end
  for _, current in pairs(self.workflow.contexts) do
    if current.history and current.history:event(event) then return end
  end
  if self.current.history and self.current.history:event(event) then return end
  if self.workflow:event(event) then self:changed(); return end
  if event.type == 'daemon.ready' then
    self.ever_connected = true
    self:request({type='integration.configure',workspace=self.launch_workspace,mode='sane'},
      'integration.configured', function(response)
        if not response then self.waiting = {}; return end
        self.configured = true
        if self.client.recovering then
          self.waiting = {}
          self.client:recover_session()
          return
        end
        local waiting = self.waiting; self.waiting = {}
        for _, action in ipairs(waiting) do action() end
      end)
  elseif event.type == 'connection.closed' then
    self:history():invalidate()
    self.configured, self.switching, self.transfer = false, false, nil
    self.current.busy, self.current.pending = false, nil
    self.waiting, self.permissions, self.permission_prompt = {}, {}, nil
    self.current.notice = event.error
  elseif event.type == 'mode.updated' or event.type == 'integration.configured' then
    self.current.mode = event.mode or self.current.mode
  elseif event.type == 'session.attach_started' and (self.switching or self.client.recovering) then
    local operation = self.client.operations[event.requestId]
    if event.requestId ~= self.attach_request and not (operation and operation.type == 'session.attach') then return end
    local ok, transfer = pcall(History.transfer,event)
    if not ok then self.client.transport:stop('Invalid attachment header'); return end
    self.transfer = transfer
  elseif self.transfer and (event.type == 'session.history_chunk' or event.type == 'session.attached') then
    local ok, err = pcall(accept_transfer, self, event)
    if not ok then self.client.transport:stop('Invalid attachment: ' .. err); return end
  elseif event.type == 'execution.completed' or event.type == 'execution.failed' or event.type == 'execution.cancelled' then
    self.current.busy, self.current.pending = false, nil
  elseif event.type == 'permission.requested' then
    self.permissions[event.permissionId] = event
    require('sos.actions').permission(self)
  elseif event.type == 'permission.resolved' or event.type == 'permission.dismissed' then
    self.permissions[event.permissionId] = nil
    if self.permission_prompt == event.permissionId then self.permission_prompt = nil end
  elseif event.type == 'execution.cancel_ignored' then
    self.client.operations[event.requestId] = nil
    self.client.transport.pending[event.requestId] = nil
  end
  if event.error and event.error ~= '' then self.current.notice = event.error end
  if event.type == 'execution.cancelled' then
    self.current.notice = (event.error or 'Execution cancelled')
      ..(self.current.permission_decision and ('\n'..self.current.permission_decision) or '')
  end
  if event.messages and (not event.sessionId or event.sessionId == self.current.id) then
    self:history():live(event)
  end
  self:changed()
end

return M
