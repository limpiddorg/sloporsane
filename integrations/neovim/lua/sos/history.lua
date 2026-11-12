local M = {}
M.__index = M
local function summary(record)
  local result = vim.deepcopy(record)
  result.activities = nil
  return result
end

function M.transfer(header)
  assert(type(header.messageCount) == 'number' and header.messageCount >= 0, 'Invalid history count')
  return {header=header, records={}, fragments={}, part=0}
end

function M.accept(transfer, event)
  local header = transfer.header
  assert(event.requestId == header.requestId and event.sessionId == header.sessionId, 'Mismatched history transfer')
  if event.type == 'session.history_chunk' then
    assert(event.messageIndex == #transfer.records and event.partIndex == transfer.part, 'Out-of-order history')
    assert(type(event.data) == 'string', 'Invalid history fragment')
    table.insert(transfer.fragments,event.data)
    transfer.part = transfer.part + 1
    if event.last then
      local record = vim.json.decode(table.concat(transfer.fragments))
      assert(type(record) == 'table' and type(record.role) == 'string', 'Invalid history record')
      table.insert(transfer.records,record)
      transfer.fragments, transfer.part = {}, 0
      assert(#transfer.records <= header.messageCount, 'Excess history records')
    end
  elseif event.type == 'session.attached' then
    assert(transfer.part == 0 and #transfer.records == event.messageCount
      and event.messageCount == header.messageCount, 'Incomplete history')
    return transfer.records
  end
end

function M.new(session, current)
  return setmetatable({session=session,current=current,page={},pending={},cache={},generation=0}, M)
end

function M:changed() self.session:changed() end

function M:invalidate()
  self.generation = self.generation + 1
  for id in pairs(self.pending) do
    self.session.client.transport.pending[id] = nil
    self.session.client.operations[id] = nil
  end
  self.pending = {}
  for _, cache in pairs(self.cache) do cache.loading = {} end
  self.loading = false
end

function M:purge()
  self:invalidate()
  self.cache = {}
  self.calls, self.group = {}, nil
  self.current.fold_reset = (self.current.fold_reset or 0) + 1
  for _, record in ipairs(self.current.records) do
    if record.role == 'activity' then record.activities = nil end
  end
end

function M:install(records, page)
  local retained = {}
  for _, record in ipairs(records) do
    if record.role == 'activity' then
      local cached = self.cache[record.id]
      -- Details survive paging only while their lightweight row list is unchanged.
      if cached and vim.deep_equal(cached.signature,summary(record))
        and (not record.activities or vim.deep_equal(record.activities,cached.summary)) then
        retained[record.id] = cached
      end
    end
  end
  self.cache = retained
  self.current.records, self.page = records, page or {}
  self.group, self.calls = nil, {}
  for _, record in ipairs(records) do
    if record.role == 'activity' then
      for _, item in ipairs(record.activities or {}) do
        if item.callId then self.calls[item.callId] = {group=record,item=item} end
      end
    end
  end
  local last = records[#records]
  if last and last.role == 'activity' then self.group = last end
end

function M:request(fields, expected, done)
  local frame = vim.tbl_extend('force',{sessionId=self.current.id},fields)
  local id, err
  id, err = self.session.client:request(frame,'session.attached',function(response,failure)
    if response and response.type ~= 'request.failed' then return end
    -- Terminal success is assembled in event(), after the client callback.
    local request = id and self.pending[id]
    if request then
      self.pending[id] = nil
      self.loading = false
      done(nil,failure or (response and response.error) or 'Disconnected')
      self:changed()
    end
  end)
  if not id then done(nil,err); return end
  self.pending[id] = {expected=expected,done=done,generation=self.generation}
  return id
end

function M:event(event)
  local request = self.pending[event.requestId]
  if not request then return false end
  if event.type == 'request.failed' then return true end
  if event.type ~= 'session.attach_started' and event.type ~= 'session.history_chunk' and event.type ~= 'session.attached' then return false end
  local ok, err = pcall(function()
    if event.type == 'session.attach_started' then
      assert(not request.transfer and event.sessionId == self.current.id, 'Unexpected history header')
      for key, value in pairs(request.expected) do assert(event[key] == value, 'Wrong history purpose or selection') end
      request.transfer = M.transfer(event)
    else
      assert(request.transfer, 'Missing history header')
      local records = M.accept(request.transfer,event)
      if records then
        self.pending[event.requestId] = nil
        request.done(records,nil,request.transfer.header)
      end
    end
  end)
  if not ok then self.session.client.transport:stop('Invalid history transfer: ' .. err) end
  self:changed()
  return true
end

function M:navigate(direction, done)
  if not self.current.id or self.session.switching then return end
  if direction ~= 'latest' and (self.loading or (direction == 'before' and not self.page.hasOlder)
    or (direction == 'after' and not self.page.hasNewer)) then return end
  self:invalidate()
  self.loading = true
  self.live_queue = {}
  local anchor = direction == 'before' and self.page.first or self.page.last
  self:request({type='session.history',direction=direction,anchor=anchor or ''},{purpose='page'},function(records,err,header)
    self.loading = false
    if records then self:install(records,header.page)
    else self.current.notice = 'History unavailable: ' .. tostring(err) end
    if not self.page.hasNewer then
      local queued = self.live_queue or {}
      for _, event in ipairs(queued) do self:live(event) end
    end
    self.live_queue = nil
    if done then done(records ~= nil) end
  end)
  self:changed()
end

function M:group_record(id)
  for _, record in ipairs(self.current.records) do
    if record.role == 'activity' and record.id == id then return record end
  end
end

function M:rows(record)
  local cached = self.cache[record.id]
  return cached and cached.rows or record.activities
end

function M:load(group, index)
  local record = self:group_record(group)
  if not record then return end
  local cache = self.cache[group]
  if not cache then
    cache = {signature=summary(record),rows=record.activities,summary=vim.deepcopy(record.activities),details={},errors={},loading={}}
    self.cache[group] = cache
  end
  if index ~= nil then
    for _, item in ipairs(record.activities or {}) do
      if item.activityIndex == index and item.live then
        cache.details[index] = item
        return
      end
    end
  end
  local key = index == nil and 'group' or index
  if cache.loading[key] or (index == nil and cache.rows) or (index ~= nil and cache.details[index]) then return end
  cache.errors[key], cache.loading[key] = nil, true
  local fields = {type=index == nil and 'session.activities' or 'session.activity_detail',groupId=group}
  local expected = {purpose=index == nil and 'activities' or 'activity-detail',groupId=group}
  if index ~= nil then fields.activityIndex=index; expected.activityIndex=index end
  self:request(fields,expected,function(records,err)
    cache.loading[key] = nil
    if not records then cache.errors[key] = tostring(err); return end
    assert(#records == 1 and records[1].id == group and records[1].role == 'activity'
      and type(records[1].activities) == 'table', 'Mismatched Activity')
    if index == nil then
      cache.rows = records[1].activities
      cache.summary = vim.deepcopy(cache.rows)
    else
      local items = records[1].activities
      assert(#items == 1 and items[1].activityIndex == index,'Mismatched Activity detail')
      cache.details[index] = items[1]
    end
  end)
  self:changed()
end

local function ensure_group(self, message, part)
  if self.group then return self.group end
  local group = {id=message.id .. '/activity/' .. tostring(part),role='activity',content={},activities={},count=0,terminal=true}
  table.insert(self.current.records,group)
  self.group = group
  return group
end

function M:live(event)
  if self.loading then table.insert(self.live_queue,event); return end
  if self.page.hasNewer then self.unseen = true; return end
  self.calls = self.calls or {}
  for _, message in ipairs(event.messages or {}) do
    -- Snapshot/live overlap uses source IDs; mixed text parts share one source ID.
    local seen = false
    for _, record in ipairs(self.current.records) do
      if record.id == message.id then seen = true; break end
    end
    if not seen and message.role ~= 'system' then
      if message.role == 'user' then
        table.insert(self.current.records,vim.deepcopy(message))
        self.group = nil
        ensure_group(self,message,0)
      elseif message.role == 'activity' then
        table.insert(self.current.records,vim.deepcopy(message))
      else
        for part, block in ipairs(message.content or {}) do
          if block.type == 'text' and block.text and block.text:match('%S') and message.role == 'assistant' then
            local record = vim.deepcopy(message)
            record.content = {vim.deepcopy(block)}
            table.insert(self.current.records,record)
            self.group = nil
          elseif block.type == 'tool_result' then
            local call = self.calls[block.callId]
            if call then
              call.item.result = vim.deepcopy(block)
              local cache = self.cache[call.group.id]
              if cache then cache.details[call.item.activityIndex] = vim.deepcopy(call.item) end
            end
          elseif block.type == 'tool_call' or (block.type == 'reasoning' and type(block.text) == 'string' and block.text ~= '') then
            local group = ensure_group(self,message,part-1)
            group.activities = group.activities or {}
            local item = {live=true,activityIndex=#group.activities,kind=block.type == 'tool_call' and 'tool' or 'reasoning',
              callId=block.callId,toolName=block.toolName,arguments=vim.deepcopy(block.arguments),text=block.text}
            table.insert(group.activities,item)
            group.count = #group.activities
            if block.callId then self.calls[block.callId] = {group=group,item=item} end
          end
        end
      end
    end
  end
  -- Bound visible messages by source identity, including all fragments of a message.
  local order, seen = {}, {}
  for _, record in ipairs(self.current.records) do
    if (record.role == 'user' or record.role == 'assistant') and not seen[record.id] then
      table.insert(order,record.id); seen[record.id]=true
    end
  end
  if #order > 20 then
    local remove = {}; for i=1,#order-20 do remove[order[i]]=true end
    local start = 1
    while start <= #self.current.records do
      local record = self.current.records[start]
      if not remove[record.id] and record.role ~= 'activity' then break end
      self.cache[record.id] = nil
      start = start + 1
    end
    self.current.records = {unpack(self.current.records,start)}
    local groups = {}
    for _, record in ipairs(self.current.records) do if record.role == 'activity' then groups[record.id]=true end end
    for call, ref in pairs(self.calls) do if not groups[ref.group.id] then self.calls[call]=nil end end
    if self.group and not groups[self.group.id] then self.group=nil end
    self.page.hasOlder = true
  end
  for _, record in ipairs(self.current.records) do
    if record.role == 'user' or record.role == 'assistant' then
      self.page.first = record.id; break
    end
  end
  for i=#self.current.records,1,-1 do
    local record = self.current.records[i]
    if record.role == 'user' or record.role == 'assistant' then self.page.last=record.id; break end
  end
end

return M
