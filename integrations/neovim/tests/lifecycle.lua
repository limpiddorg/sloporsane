-- Controlled-provider real daemon scenario; launched by smoke.py --lifecycle.
vim.opt.runtimepath:prepend(vim.fn.getcwd()..'/integrations/neovim')
local ok,err=xpcall(function()
  local client=require('sos.client').new({socket=vim.env.SOS_TEST_SOCKET,auto_start=false,timeout_ms=10000})
  local events={}
  client.on_event=function(event) events[#events+1]=event end
  local s=require('sos.session').new(client,{workspace=vim.env.SOS_TEST_WORKSPACE})
  local w=s.workflow
  local function wait(check)
    if not vim.wait(25000,check,10) then
      local recent={}
      for n=math.max(1,#events-15),#events do
        local event=events[n]
        recent[#recent+1]={type=event.type,error=event.error,status=event.implementation and event.implementation.status,
          requestId=event.requestId}
      end
      error('Lifecycle timeout: '..vim.inspect(recent))
    end
  end
  local function command(frame,terminal)
    local done,response,failure
    assert(s:request(frame,terminal,function(event,error) response,failure,done=event,error,true end))
    wait(function() return done end); assert(response,failure); return response
  end
  vim.ui.select=function(rows,_,callback)
    for _,row in ipairs(rows) do if type(row)=='table' and row.decision=='yes' then callback(row); return end end
    callback(rows[2] or rows[1])
  end
  client:connect(); wait(function() return s.configured end)
  local provider=command({type='provider.create',name='Lifecycle fixture',base_url=vim.env.SOS_TEST_URL},'provider.listed').created_provider
  command({type='provider.select',provider=provider},'provider.selected')
  command({type='model.list'},'model.listed')
  command({type='model.select',provider=provider,model='local-model',context_limit=65536},'model.selected')
  s:set_draft('Build the controlled fixture'); s:send()
  wait(function() return s.current.id and not s.current.pending and not s.current.busy and #(w.info.objective_plans or {})>0 end)
  local objective=w.info.objective_plans[1]
  w:open_objective(objective,false)
  wait(function() return w.branch_id and not s.switching and w.info.implementation_plan_ready and not s.current.busy end)
  command({type='implementation.start',sessionId=w.branch_id,pace='step_by_step'},'implementation.updated')
  wait(function() return w.state and w.state.steps[1].whole.complete end)
  local path=vim.env.SOS_TEST_WORKSPACE..'/state.txt'
  local function original() return table.concat(vim.fn.readfile(path),'\n')..'\n' end
  assert(original()=='original\n','Construction leaked into the original workspace')
  w:select_step(w.state.steps[1].identity); s:inspect()
  assert(s.inspector.source.text=='built\n')
  s:review_question(); s:set_draft('Correct the fixture state'); s:send()
  wait(function() return type(w.state.proposed_rework)=='string' and w.state.proposed_rework~=''
    and not s.current.busy and not s.current.pending end)
  assert(original()=='original\n','Review modified the original workspace')
  s:accept_proposal()
  wait(function() return w.state.steps[1].whole.complete and (w.state.history_count or 0)>0
    and w.state.proposed_rework=='' and not s:review_actions().pending end)
  w:select_step(w.state.steps[1].identity); s:inspect()
   assert(s.inspector.source.text=='corrected\n')
   s:next_change(); s:previous_change(); s:next_substep(); s:previous_substep()
   assert(original()=='original\n','Navigation imported files')
   s:versions()
   wait(function() return w.archive_view end)
   assert(s.inspector.source.text=='built\n','Archive inspection read corrected checkout contents')
   assert(original()=='original\n','Archive inspection imported files')
   s:review_actions():live()
   w:select_step(w.state.steps[1].identity); s:inspect()
   assert(s.inspector.source.text=='corrected\n')
   assert(original()=='original\n','Correction was imported without human validation')
  s:indicator()
  wait(function() return w.state.steps[1].substeps[1].reviewed and not s:review_actions().pending end)
  assert(original()=='original\n','Indicator toggle imported files')
  s:validate()
  wait(function() return w.state.steps[1].whole.reviewed and not s:review_actions().pending end)
  assert(original()=='corrected\n','Explicit final validation did not import corrected evidence')
  s.inspector:hide(); client:close()
end,debug.traceback)
if not ok then io.stderr:write(err..'\n'); vim.cmd('cquit 1') end
print('Real-daemon planning/construction/inspection/correction/import passed')
vim.cmd('qa!')
