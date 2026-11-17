local Session=require('sos.session')
local Render=require('sos.render')
local function fake(ready)
  local client={transport={state=ready==false and 'starting' or 'ready',pending={}},conversation={},operations={},sent={}}
  function client:connect() end
  function client:request(frame,terminal,callback)
    if self.fail then return nil,'Write failed' end
    local id=tostring(#self.sent+1)
    self.sent[#self.sent+1]={id=id,frame=frame,terminal=terminal,callback=callback}
    return id
  end
  function client:reply(event)
    for _,request in ipairs(self.sent) do
      if request.id==event.requestId and (event.type==request.terminal or event.type=='request.failed') then
        if request.callback then request.callback(event) end
        break
      end
    end
    self.on_event(event)
  end
  local session=Session.new(client,{workspace=vim.fn.getcwd()})
  session.configured=ready~=false
  return session,client
end
local function send(session,text)
  session:set_draft(text)
  session:send()
end
local function accept(session,client)
  local request=client.sent[#client.sent]
  local message={id='user:'..request.id,role='user',content={{type='text',text=request.frame.text}},metadata={mode='sane'}}
  client:reply({type=session.current.id and 'message.accepted' or 'session.created',requestId=request.id,
    session={id='discussion'},sessionId='discussion',messages={message}})
end
local function text(session)
  return table.concat(Render.project(session.current,session.info,80).lines,'\n')
end
local s,c=fake()
send(s,'first')
assert(s.current.busy and s.current.pending and s.current.draft=='')
assert(#s.current.records==2 and s.current.records[1].role=='user')
local activity=s.current.records[2]
local started=activity._started_at
assert(started and text(s):find('first',1,true))
assert(Render.project(s.current,s.info,80).running_activity)
assert(not s:history().page.first,'Provisional ID escaped into history navigation')
s:set_draft('new draft')
accept(s,c)
assert(#s.current.records==2 and s.current.records[2]==activity and activity._started_at==started)
assert(s.current.records[1].id=='user:1' and s.current.draft=='new draft')
c:reply({type='provider.message_arrived',sessionId='discussion',messages={s.current.records[1]}})
assert(#s.current.records==2,'Accepted user was duplicated')
send(s,'next')
send(s,'also next')
assert(#c.sent==1 and s.current.queued=='next\nalso next' and s.current.draft=='')
assert(text(s):find('[Pending]',1,true) and text(s):find('also next',1,true))
s:set_draft('keep editing')
c:reply({type='execution.completed',sessionId='other'})
assert(s.current.busy and #c.sent==1,'Foreign terminal event drained the queue')
c:reply({type='execution.completed',sessionId='discussion'})
assert(vim.wait(1000,function() return #c.sent==2 end,10))
assert(c.sent[2].frame.text=='next\nalso next' and s.current.draft=='keep editing')
assert(not s.current.queued and s.current.busy and not text(s):find('[Pending]',1,true))
accept(s,c)
c:reply({type='execution.completed',sessionId='discussion'})
assert(#s.current.records==4)

-- Rejections restore submitted text, the queue, then the newer draft in order.
for _,failure in ipairs({'request','write','disconnect','cancel'}) do
  local session,client=fake()
  if failure=='write' then client.fail=true end
  send(session,'submitted')
  if failure=='write' then
    assert(session.current.draft=='submitted' and #session.current.records==0)
  else
    send(session,'queued')
    session:set_draft('editing')
    if failure=='request' then client:reply({type='request.failed',requestId='1',error='Rejected'})
    elseif failure=='disconnect' then
      client.transport.state='disconnected'
      client:reply({type='connection.closed',error='Disconnected'})
    else client:reply({type='execution.cancelled'}) end
    assert(session.current.draft=='submitted\nqueued\nediting',failure)
    assert(not session.current.queued and not session.current.busy and not session.current.pending)
    assert(#session.current.records==0 and not Render.project(session.current,nil,80).running_activity)
  end
end

-- Accepted turns remain historical; only unsent next-turn text is restored.
for _,kind in ipairs({'execution.failed','execution.cancelled','connection.closed'}) do
  local session,client=fake()
  send(session,'accepted'); accept(session,client)
  send(session,'queued'); session:set_draft('editing')
  client:reply({type=kind,sessionId='discussion'})
  assert(session.current.draft=='queued\nediting' and #session.current.records==2)
  assert(not session.current.queued and #client.sent==1)
end

-- A context switch between completion and scheduled dispatch cannot send elsewhere.
local switch,sc=fake()
send(switch,'accepted'); accept(switch,sc); send(switch,'queued')
local old=switch.current
sc:reply({type='execution.completed',sessionId='discussion'})
switch.current={draft='elsewhere',records={},mode='sane',workspace=vim.fn.getcwd()}
assert(vim.wait(1000,function() return not old.queued end,10))
assert(old.draft=='queued' and #sc.sent==1 and switch.current.draft=='elsewhere')

-- Immediate feedback also covers daemon startup, before any network request.
local startup,st=fake(false)
send(startup,'starting')
assert(#st.sent==0 and startup.current.busy and Render.project(startup.current,nil,80).running_activity)
st.transport.state='ready'
st:reply({type='daemon.ready'})
st:reply({type='integration.configured',requestId='1',mode='sane'})
assert(#st.sent==2 and st.sent[2].frame.text=='starting')
print('Immediate optimistic turns, identity reconciliation and local pending-message lifecycle passed')
