local Session = require('sos.session')
local Panel = require('sos.panel')
local History = require('sos.history')
local Render = require('sos.render')
local client = {transport={state='ready',pending={}},conversation={},operations={},sent={}}
function client:request(frame,terminal,callback)
  local id = tostring(#self.sent+1)
  self.sent[#self.sent+1] = {id=id,frame=frame,terminal=terminal,callback=callback}
  self.transport.pending[id] = true
  self.operations[id] = frame
  return id
end
function client:reply(event)
  local request = self.sent[tonumber(event.requestId)]
  if request and self.transport.pending[event.requestId] and (event.type == request.terminal or event.type == 'request.failed') then
    self.transport.pending[event.requestId] = nil
    self.operations[event.requestId] = nil
    request.callback(event,event.error)
  end
  self.on_event(event)
end
local session = Session.new(client,{workspace=vim.fn.getcwd()})
session.configured = true
session.current.id = 'history-session'
local history = session:history()
local function message(id, body)
  return {id=id,role='assistant',content={{type='text',text=body or id}}}
end
local function deliver(id, records, selection, page)
  local header = vim.tbl_extend('force',{type='session.attach_started',requestId=id,sessionId=session.current.id,
    messageCount=#records,purpose='page',page=page or {}}, selection or {})
  client:reply(header)
  for n, record in ipairs(records) do
    local data = vim.json.encode(record)
    local split = math.floor(#data/2)
    client:reply({type='session.history_chunk',requestId=id,sessionId=session.current.id,
      messageIndex=n-1,partIndex=0,last=false,data=data:sub(1,split)})
    client:reply({type='session.history_chunk',requestId=id,sessionId=session.current.id,
      messageIndex=n-1,partIndex=1,last=true,data=data:sub(split+1)})
  end
  client:reply({type='session.attached',requestId=id,sessionId=session.current.id,messageCount=#records})
end
local activity = {id='u1/activity/0',role='activity',content={},count=2,kinds={'tools'}}
local long_message = message('m1',string.rep('line\n',40))
history:install({long_message,activity,message('m2')},{first='m1',last='m2',hasOlder=true,hasNewer=false})
local panel = Panel.new(session)
panel:show()
assert(#client.sent == 0, 'Rendering eagerly requested details')
local function span(key)
  for _, item in ipairs(session.current.view.projection.spans) do if item.key == key then return item end end
  error('Missing span '..key)
end
local function fold(key, command)
  local selected = span(key)
  vim.api.nvim_win_call(panel.transcript,function()
    vim.api.nvim_win_set_cursor(panel.transcript,{selected.start,0})
    vim.cmd('normal! '..command)
  end)
  panel:sync_folds()
end
local key = 'activity:'..activity.id
fold(key,'zo')
assert(#client.sent == 1 and client.sent[1].frame.type == 'session.activities')
local rows = {
  {kind='tool',toolName='bash',callId='call',activityIndex=0,arguments={mainCommand='pytest'},result={isError=false}},
  {kind='reasoning',activityIndex=1},
}
deliver('1',{{id=activity.id,role='activity',content={},activities=rows}},{purpose='activities',groupId=activity.id})
assert(#client.sent == 1 and history.cache[activity.id].rows[1].arguments.mainCommand == 'pytest')
assert(vim.api.nvim_win_call(panel.transcript,function() return vim.fn.foldclosed(span(key).start) end) == -1)
fold(key..'/item/0','zo')
assert(#client.sent == 2 and client.sent[2].frame.activityIndex == 0)
deliver('2',{{id=activity.id,role='activity',content={},activities={{kind='tool',toolName='bash',callId='call',activityIndex=0,
  arguments={command='pytest -q'},result={isError=false,output='controlled output'}}}}},
  {purpose='activity-detail',groupId=activity.id,activityIndex=0})
local transcript = table.concat(session.current.view.projection.lines,'\n')
assert(transcript:find('controlled output',1,true) and not transcript:find('Expand to load activities',1,true))
assert(not history.cache[activity.id].details[1], 'Unrelated reasoning detail loaded')
local before = #client.sent
panel:sync_folds(); panel:sync_folds()
assert(#client.sent == before)
-- Retained records keep both native folds and loaded details across an overlapping page.
vim.api.nvim_win_call(panel.transcript,function() vim.fn.winrestview({lnum=15,topline=10}) end)
local anchor_before = session.current.view.projection.line_keys[15]
history:navigate('before')
assert(client.sent[#client.sent].frame.anchor == 'm1')
local page_id = client.sent[#client.sent].id
deliver(page_id,{message('m0'),long_message,vim.deepcopy(activity),message('m2')},nil,
  {first='m0',last='m2',hasOlder=false,hasNewer=true})
assert(history.cache[activity.id].details[0] and session.current.records[1].id == 'm0')
local cursor_after = vim.api.nvim_win_get_cursor(panel.transcript)[1]
assert(session.current.view.projection.line_keys[cursor_after] == anchor_before, 'Page replacement lost the reading anchor')
assert(vim.api.nvim_win_call(panel.transcript,function() return vim.fn.foldclosed(span(key..'/item/0').start) end) == -1)
local old = vim.deepcopy(session.current.records)
client:reply({type='provider.message_arrived',sessionId=session.current.id,messages={message('new-live')}})
assert(vim.deep_equal(old,session.current.records), 'Live events moved an older page')
-- Failed page replacement keeps the previous records and is retryable.
history:navigate('after')
client:reply({type='request.failed',requestId=client.sent[#client.sent].id,error='Fixture unavailable'})
assert(vim.deep_equal(old,session.current.records) and not history.loading)
-- Late detail replies cannot repopulate purged data.
fold(key..'/item/1','zo')
local stale = client.sent[#client.sent].id
history:purge(); session:changed()
deliver(stale,{{id=activity.id,role='activity',activities={{kind='reasoning',activityIndex=1,text='stale'}}}},
  {purpose='activity-detail',groupId=activity.id,activityIndex=1})
assert(next(history.cache) == nil)
-- Stale pages cannot replace a newer page request.
history:navigate('latest')
local stale_page = client.sent[#client.sent].id
history:navigate('latest')
local latest = client.sent[#client.sent].id
deliver(stale_page,{message('wrong')})
assert(session.current.records[1].id ~= 'wrong')
deliver(latest,{message('latest')},nil,{first='latest',last='latest',hasOlder=true,hasNewer=false})
assert(session.current.records[1].id == 'latest')
-- A failed detail waits for explicit close/reopen, not idle-triggered retries.
history:install({activity},{hasNewer=false}); session:changed()
fold(key,'zo')
local fail_id = client.sent[#client.sent].id
client:reply({type='request.failed',requestId=fail_id,error='Detail failed'})
before=#client.sent; panel:sync_folds(); panel:sync_folds(); assert(#client.sent==before)
fold(key,'zc'); fold(key,'zo')
assert(#client.sent==before+1)
-- Changed lightweight summaries invalidate bodies; canceled loads release markers.
local changed_activity = vim.deepcopy(activity)
changed_activity.count = 3
history.cache[activity.id] = {signature=vim.deepcopy(activity),summary=nil,rows=rows,
  details={[0]={text='old'}},errors={},loading={[1]=true}}
history:invalidate()
assert(not history.cache[activity.id].loading[1])
history:install({changed_activity},{})
assert(not history.cache[activity.id])

-- Malformed transfers are rejected; no partial records are installed.
local transfer = History.transfer({requestId='x',sessionId='s',messageCount=1})
assert(not pcall(History.accept,transfer,{type='session.history_chunk',requestId='x',sessionId='s',messageIndex=1,partIndex=0,data='{}',last=true}))
-- Historical objective versions remain tied to their own completed turns.
local projected = Render.project({records={message('turn-a'),message('turn-b')}},{
  objective_plans={{id='p1',content='Version one',objective_stack='Lua'},{id='p2',content='Version two'}},
  objective_plan_turns={{message_id='turn-a',objective_id='p1',updated=false},{message_id='turn-b',objective_id='p2',updated=true}},
})
assert(table.concat(projected.lines,'\n'):find('Version one',1,true) and table.concat(projected.lines,'\n'):find('Version two',1,true))
assert(#projected.spans == 2 and projected.spans[1].key ~= projected.spans[2].key)
-- Bound live text by message identity, and keep tool results paired with calls.
history:invalidate(); history:install({}, {})
for n=1,25 do history:live({messages={message('live-'..n)}}) end
assert(#session.current.records==20 and session.current.records[1].id=='live-6' and history.page.hasOlder)
history:live({messages={{id='u',role='user',content={{type='text',text='question'}}}}})
history:live({messages={{id='a',role='assistant',content={{type='tool_call',callId='c',toolName='bash',arguments={mainCommand='echo',command='echo safe'}}}}}})
history:live({messages={{id='t',role='tool',content={{type='tool_result',callId='c',output='safe',isError=false}}}}})
assert(history.group.id == 'u/activity/0' and history.group.activities[1].result.output=='safe')
panel:hide()
print('Bounded history and native fold tests passed')
