local Session=require('sos.session')
local client={transport={state='ready',pending={}},conversation={},operations={},sent={}}
function client:request(frame,terminal,callback)
  local id=tostring(#self.sent+1); self.sent[#self.sent+1]={frame=frame,terminal=terminal,callback=callback}; return id
end
function client:reply(id,event)
  event.requestId=id
  self.sent[tonumber(id)].callback(event)
  self.on_event(event)
end
local s=Session.new(client,{workspace='/original'}); s.configured=true
local w=s.workflow
w.branch_id='branch'; w.discussion_id='discussion'; w.selected_identity='first'
w.info={workspace='/original'}
w.state={revision=3,status='completed',review_step=0,plan={steps={{title='First',substeps={{title='Part'}}}}},
  steps={{identity='first',generation=1,substeps={{complete=true,reviewed=false,diffs={}}},whole={complete=true,reviewed=false,diffs={}}}}}
w:review(0,0)
local review=s:review_actions()
local select=vim.ui.select
local held,prompt
vim.ui.select=function(rows,options,callback) held=function() callback(rows[2]) end; prompt=options.prompt end
s:indicator(); local id=tostring(#client.sent)
assert(client.sent[tonumber(id)].frame.substep==0 and client.sent[tonumber(id)].frame.revision==3)
local count=#client.sent; s:indicator(); assert(#client.sent==count)
client:reply(id,{type='request.failed',error='Stale revision'})
assert(not w.state.steps[1].substeps[1].reviewed and s.current.substep==0)
s:validate(); assert(prompt:find('1 unchecked') and #client.sent==count)
s:validate(); assert(#client.sent==count)
held(); id=tostring(#client.sent)
assert(client.sent[tonumber(id)].frame.substep==1,'Final import reused the indicator selection')
client:reply(id,{type='request.failed',error='Import conflict'})
assert(w.state.review_step==0 and not w.state.steps[1].whole.reviewed and s.current.substep==0)
s:validate(); w.state.revision=4; count=#client.sent; held(); assert(#client.sent==count,'Stale confirmation submitted')
s:indicator(); id=tostring(#client.sent)
local updated=vim.deepcopy(w.state); updated.steps[1].substeps[1].reviewed=true
client:reply(id,{type='implementation.updated',sessionId='branch',implementation=updated})
assert(w.state.steps[1].substeps[1].reviewed)
s:indicator(); id=tostring(#client.sent); assert(client.sent[tonumber(id)].frame.type=='implementation.unvalidate')
client:reply(id,{type='request.failed',error='Unavailable'})
-- Proposals require current tokens; opening revised discussion is not execution.
w.state.proposed_rework='Change the approach'; w.state.proposed_plan_markdown='Candidate plan'
w.state.revision=5
s:proposal(); assert(w:readonly(s.current) and s.current.records[1].content:find('Candidate plan'))
s:accept_proposal(); w.state.revision=6; count=#client.sent; held(); assert(#client.sent==count)
s:accept_proposal(); held(); id=tostring(#client.sent)
assert(client.sent[tonumber(id)].frame.type=='implementation.revision.open')
updated=vim.deepcopy(w.state); updated.revision_planning=true; updated.status='revision_planning'
client:reply(id,{type='implementation.updated',sessionId='branch',implementation=updated})
assert(s.current.kind=='revision')
s.current.draft='Question about candidate'; s:send(); id=tostring(#client.sent)
assert(client.sent[tonumber(id)].frame.type=='implementation.revision.review')
s.current.draft='Keep typed later'
client:reply(id,{type='implementation.revision.review.accepted'})
assert(s.current.draft=='Keep typed later')
client.on_event({type='btw.started',sessionId='branch',requestId='candidate',messageCount=0,branch={revision_planning=true,status='completed'}})
client.on_event({type='btw.finished',sessionId='branch',requestId='candidate',messageCount=0})
assert(not s.current.busy)
s:accept_proposal(); held(); id=tostring(#client.sent)
assert(client.sent[tonumber(id)].frame.type=='implementation.rework' and client.sent[tonumber(id)].frame.revision==6)
client:reply(id,{type='request.failed',error='Correction preparation failed'})
assert(w.state.revision_planning and s.current.draft=='Keep typed later')
s:reject_proposal(); held(); id=tostring(#client.sent)
assert(client.sent[tonumber(id)].frame.type=='implementation.rework.reject')
client:reply(id,{type='request.failed',error='Try later'})
-- Archive browsing must not overwrite live state or permit mutation.
local archived=vim.deepcopy(w.state); archived.status='archived'; archived.revision_planning=false
archived.steps[1].whole.explanation='Immutable old evidence'
s:versions(); id=tostring(#client.sent)
client:reply(id,{type='implementation.history',implementations={archived}})
held(); assert(w.archive_view and w.state.status=='archived')
count=#client.sent; s:indicator(); s:validate(); s:accept_proposal(); s:send(); s:cancel(); assert(#client.sent==count)
local live=vim.deepcopy(updated); live.revision=6
client.on_event({type='implementation.updated',sessionId='branch',implementation=live})
assert(w.state.status=='archived' and w.live_state.revision==6)
review:live(); assert(w.state.revision==6 and not w.archive_view)
if s.inspector then s.inspector:hide() end
vim.ui.select=select
print('Revision-safe review/import/proposal/archive tests passed')
