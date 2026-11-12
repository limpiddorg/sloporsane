local Session = require('sos.session')
local Panel = require('sos.panel')
local Actions = require('sos.actions')
local function fake()
  local client = {transport={state='ready',pending={}},conversation={},operations={},sent={},seq=0}
  function client:connect() end
  function client:request(frame, terminal, callback)
    self.seq = self.seq + 1
    local id = tostring(self.seq)
    table.insert(self.sent, {frame=frame,id=id,terminal=terminal,callback=callback})
    return id
  end
  function client:reply(event)
    for _, request in ipairs(self.sent) do
      if request.id == event.requestId and (event.type == request.terminal or event.type == 'request.failed') then
        if request.callback then request.callback(event,event.error) end
        break
      end
    end
    self.on_event(event)
  end
  local session = Session.new(client,{workspace=vim.fn.getcwd()})
  session.configured = true
  return session,client
end
local session, client = fake()
session.info = {active_implementation_id='',implementation_view=false}
local panel = Panel.new(session)
local origin = vim.api.nvim_get_current_win()
local buf = vim.api.nvim_get_current_buf()
local cwd = vim.fn.getcwd()
panel:show()
assert(vim.api.nvim_get_current_win() == origin and vim.api.nvim_get_current_buf() == buf)
local input = session.current.view.input
vim.api.nvim_buf_set_lines(input,0,-1,false,{'hello','world'})
assert(session.current.draft == 'hello\nworld')
session:send()
assert(client.sent[1].frame.text == 'hello\nworld' and session.current.pending)
vim.api.nvim_buf_set_lines(input,0,-1,false,{'typed while waiting'})
client:reply({type='session.created',requestId='1',session={id='first'}})
assert(session.current.draft == 'typed while waiting' and session.current.busy)
assert(not pcall(vim.api.nvim_buf_set_lines,session.current.view.transcript,0,-1,false,{'bad'}))
client:reply({type='execution.completed',sessionId='first',messages={{id='answer',role='assistant',content={{type='text',text='answer'}}}}})
assert(not session.current.busy)
session:set_draft('accepted draft'); session:changed(); session:send()
client:reply({type='message.accepted',requestId='2'})
assert(session.current.draft == '')
local choose = vim.ui.select
local selected
vim.ui.select = function(rows, _, callback) selected={rows=rows,callback=callback} end
session:new_session()
assert(#client.sent == 2 and selected)
selected.callback(selected.rows[1]) -- Keep working
assert(#client.sent == 2)
session:new_session()
selected.callback(selected.rows[2])
assert(client.sent[3].frame.type == 'session.new')
client:reply({type='session.reset',requestId='3',workspace=cwd})
assert(session.saved.first and session.current.id == nil)
assert(client.sent[4].frame.type == 'mode.set')
client:reply({type='mode.updated',requestId='4',mode='sane'})
session:set_draft('new unsent'); session:changed()
session:attach('first')
local id = client.sent[#client.sent].id
client:reply({type='session.attach_started',requestId=id,sessionId='first',messageCount=1,
  session={id='first',workspace=cwd,mode='sane'}})
local record = vim.json.encode({id='restored',role='assistant',content={{type='text',text='restored'}}})
client:reply({type='session.history_chunk',requestId=id,sessionId='first',messageIndex=0,partIndex=0,last=true,data=record})
client:reply({type='session.attached',requestId=id,sessionId='first',messageCount=1})
assert(session.current.id == 'first' and #session.current.records == 1)
local found = false
for _, draft in pairs(session.saved) do if draft.draft == 'new unsent' then found = true end end
assert(found)
-- Mode changes are acknowledgement-only, including refused changes.
session:toggle_mode()
local mode_id = client.sent[#client.sent].id
assert(session.current.mode == 'sane')
client:reply({type='request.failed',requestId=mode_id,error='Mode locked'})
assert(session.current.mode == 'sane' and session.current.notice == 'Mode locked')
session:toggle_mode()
client:reply({type='mode.updated',requestId=client.sent[#client.sent].id,mode='slop'})
assert(session.current.mode == 'slop')
-- Reading above the bottom, hiding and reopening must preserve position/focus.
for n=1,80 do table.insert(session.current.records,{id='long-reading-message',role='assistant',content={{type='text',text='row '..n}}}) end
session:changed()
vim.api.nvim_win_call(panel.transcript,function() vim.fn.winrestview({lnum=5,topline=3}) end)
local before = vim.api.nvim_win_call(panel.transcript,vim.fn.winsaveview)
client:reply({type='provider.message_arrived',sessionId='first',messages={{id='new-answer',role='assistant',content={{type='text',text='new'}}}}})
local after = vim.api.nvim_win_call(panel.transcript,vim.fn.winsaveview)
assert(before.topline == after.topline and before.lnum == after.lnum)
panel:hide(); panel:show()
after = vim.api.nvim_win_call(panel.transcript,vim.fn.winsaveview)
assert(before.topline == after.topline and before.lnum == after.lnum, 'Reopen position changed')
assert(vim.api.nvim_get_current_win() == origin and vim.fn.getcwd() == cwd)
-- Unsupported commands stay local and retain text; supported completion is native.
session:set_draft('/not-supported')
local sent = #client.sent
Actions.slash(session)
assert(#client.sent == sent and session.current.draft == '/not-supported')
assert(vim.tbl_contains(Actions.complete(0,'/mo'),'/mode'))
-- Live permission scopes are displayed, decisions go only through permission.reply.
client:reply({type='permission.requested',permissionId='p1',tool='write',always={'*'},details={filepath='x'}})
assert(selected.rows[1].decision == 'no' and session.current.notice:find('filepath'))
selected.callback(selected.rows[2])
assert(client.sent[#client.sent].frame.decision == 'yes')
client:reply({type='permission.resolved',permissionId='p1'})
client:reply({type='permission.requested',permissionId='p2',tool='bash',always={},details={command='false'}})
selected.callback(selected.rows[1])
assert(client.sent[#client.sent].frame.decision == 'no')
client:reply({type='execution.cancelled',error='Execution cancelled'})
assert(session.current.notice:find('Permission denied for bash',1,true))
-- A dismissed prompt cannot grant a later request reusing its ID.
client:reply({type='permission.dismissed',permissionId='p2'})
client:reply({type='permission.requested',permissionId='reused',tool='write',always={},details={}})
local stale = selected
client:reply({type='permission.dismissed',permissionId='reused'})
client:reply({type='permission.requested',permissionId='reused',tool='bash',always={},details={}})
local count_before = #client.sent
stale.callback(stale.rows[2])
assert(#client.sent == count_before)
selected.callback(selected.rows[1])
assert(#client.sent == count_before + 1)

-- Null context maxima are accepted without comparisons to vim.NIL.
Actions.model(session)
local mid = client.sent[#client.sent].id
client:reply({type='model.listed',requestId=mid,models={{provider='test',model='local',max_context=vim.NIL}}})
local oldinput = vim.ui.input
vim.ui.input = function(_, callback) callback('32768') end
selected.callback(selected.rows[1])
assert(client.sent[#client.sent].frame.type == 'model.select')
vim.ui.input, vim.ui.select = oldinput, choose
panel:hide()
print('Native session/panel/action tests passed')
