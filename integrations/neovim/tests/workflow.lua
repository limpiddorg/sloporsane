local Session=require('sos.session')
local Panel=require('sos.panel')
local client={transport={state='ready',pending={}},conversation={},operations={},sent={}}
function client:request(frame,terminal,callback)
  local id=tostring(#self.sent+1)
  self.sent[#self.sent+1]={id=id,frame=frame,terminal=terminal,callback=callback}
  self.operations[id]=frame
  return id
end
function client:reply(event)
  local request=self.sent[tonumber(event.requestId)]
  if request and (event.type==request.terminal or event.type=='request.failed') then request.callback(event) end
  self.on_event(event)
end
local s=Session.new(client,{workspace='/original'})
s.configured=true
s.current.id='discussion'; s.current.draft='Discussion draft'; s:changed()
local w=s.workflow
local function attach(id,info,records)
  local request=s.attach_request
  client:reply({type='session.attach_started',requestId=request,sessionId=id,messageCount=#records,
    session={id=id,discussion_id='discussion',workspace='/original',mode='sane'},info=info})
  for n,row in ipairs(records) do client:reply({type='session.history_chunk',requestId=request,sessionId=id,
    messageIndex=n-1,partIndex=0,last=true,data=vim.json.encode(row)}) end
  client:reply({type='session.attached',requestId=request,sessionId=id,messageCount=#records,info=info})
end
local select=vim.ui.select
vim.ui.select=function(rows,_,callback) callback(rows[1]) end
w:open_objective({id='old-objective'},false)
assert(client.sent[#client.sent].frame.objectiveId=='old-objective')
local info={discussion_id='discussion',implementation_view=true,implementation_archive=false,
  workspace='/original',execution_workspace='/isolated',implementation_plan_ready=false}
attach('branch',info,{})
assert(client.sent[#client.sent].frame.implementation_request==true)
assert(client.sent[#client.sent].frame.text=='Implementation plan request')
local generate=client.sent[#client.sent]
client:reply({type='message.accepted',requestId=generate.id})
client:reply({type='execution.completed',sessionId='branch',info=vim.tbl_extend('force',info,{implementation_plan_ready=true,
  implementation_plan={content={{type='text',text='Final plan'}}}})})
s.current.draft='Planning draft'
w:show_plan(); assert(w:readonly(s.current) and s.current.records[1].content=='Final plan')
w:planning(); assert(s.current.draft=='Planning draft')
w:start(); assert(client.sent[#client.sent].frame.type=='implementation.start' and client.sent[#client.sent].frame.pace=='step_by_step')
local state={status='running',preparing=true,work_step=0,review_step=0,
  plan={steps={{title='First',objective='Build it',substeps={{title='Part'}}},{title='Second',substeps={{title='Next'}}}}},
  steps={{identity='first',generation=1,whole={complete=false,reviewed=false},validation={started=false,complete=false},substeps={{complete=false}}},
    {identity='second',generation=1,whole={complete=false,reviewed=false},validation={started=false,complete=false},substeps={{complete=false}}}}}
local function update() client:reply({type='implementation.updated',sessionId='branch',implementation=vim.deepcopy(state),info=info}) end
update(); assert(w:progress():find('Preparing'))
assert(s.current.kind=='step' and s.current.step==0)
w:planning()
local panel=Panel.new(s); panel:show()
local branch=s.current
local count=#client.sent
w:discussion()
assert(s.current.id=='discussion' and s.current.preview and s.current.draft=='Discussion draft')
assert(client.conversation.session_id=='branch' and not vim.bo[s.current.view.input].modifiable)
assert(vim.wo[panel.input].winbar:find('/original') and vim.wo[panel.input].winbar:find('/isolated'))
count=#client.sent; s:send(); assert(#client.sent==count)
s:toggle_mode(); assert(#client.sent==count)
w:planning(); assert(s.current==branch and s.current.draft=='Planning draft')
w:steps(); local menu=w.selector
vim.api.nvim_win_set_cursor(menu.window,{3,0})
local focus=panel.input
vim.api.nvim_set_current_win(focus)
state.preparing=false; state.steps[1].validation.started=true
update()
assert(vim.api.nvim_get_current_win()==focus and vim.api.nvim_win_get_cursor(menu.window)[1]==3)
assert(w:progress():find('Validating') and not w:eligible(0,1))
client:reply({type='execution.completed',sessionId='branch'})
assert(w.state.status=='running' and w:progress():find('Validating'),'Tool/turn termination invented completion')
menu:close()
state.steps[1].whole.complete=true; state.status='waiting'; update()
assert(w:progress():find('Waiting') and w:eligible(0,1) and not w:eligible(1,1))
vim.ui.select=function(rows,_,callback) callback(rows[2]) end
w:choose_review(0); assert(s.current.kind=='review' and s.current.substep==0)
vim.ui.select=function(rows,_,callback) callback(rows[1]) end
w:review(0,1); s.current.draft='Whole question'; local whole=s.current
w:review(0,0); assert(s.current.draft==''); s.current.draft='Part question'
s:send(); local review=client.sent[#client.sent]
assert(review.frame.type=='implementation.review' and review.frame.step==0 and review.frame.substep==0)
s.current.draft='Typed later'
client:reply({type='implementation.review.accepted',requestId=review.id})
assert(s.current.draft=='Typed later')
w:review(0,1); assert(s.current==whole and s.current.draft=='Whole question')
local before=branch.records
client:reply({type='btw.started',requestId='review',sessionId='branch',messageCount=1,branch={review=true,review_step=0,status='completed'}})
client:reply({type='btw.chunk',requestId='review',sessionId='branch',messageIndex=0,partIndex=0,last=true,
  data=vim.json.encode({id='reply',role='assistant',content='Review answer'})})
client:reply({type='btw.finished',requestId='review',sessionId='branch',messageCount=1})
assert(s.current.records[1].content=='Review answer' and branch.records==before)
state.status='interrupted'; update(); assert(w:progress():find('Interrupted'))
vim.ui.select=function(rows,_,callback) callback(rows[2]) end
w:resume(); assert(client.sent[#client.sent].frame.type=='implementation.resume')
state.status='running'; update(); s:cancel(); assert(client.sent[#client.sent].frame.type=='implementation.review.cancel')
w:planning(); s:cancel(); assert(client.sent[#client.sent].frame.type=='execution.cancel')
state.status='completed'; state.steps[1].whole.reviewed=true; state.steps[2].whole.complete=true
state.steps[2].whole.reviewed=true; state.steps[2].conclusion='Done'; update()
assert(w:finished()); w:conclusion(); assert(s.current.records[1].content=='Done')
w:discussion(); assert(client.sent[#client.sent].frame.type=='implementation.close')
attach('discussion',{workspace='/original',mode='sane'}, {})
assert(not w:readonly(s.current) and s.current.draft=='Discussion draft')
w.branch_id='branch'; w.info={auto_plan_pending=true,auto_plan_running=false,auto_plan_pace='everything',auto_plan_error='Saved failure'}
assert(w:progress():find('Saved failure')); w:auto_retry()
assert(client.sent[#client.sent].frame.type=='implementation.auto_plan' and client.sent[#client.sent].frame.pace=='everything')
w.info.implementation_archive=true
count=#client.sent; w:auto_retry(); assert(#client.sent==count)
panel:hide(); vim.ui.select=select
print('SOS planning/context/progress tests passed')
