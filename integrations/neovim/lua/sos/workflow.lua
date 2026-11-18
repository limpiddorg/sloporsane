local History = require('sos.history')
local M = {}
M.__index = M
local function present(value) return type(value)=='string' and value~='' end
local function context(id,kind,workspace)
  return {id=id,kind=kind,workspace=workspace or '',mode='sane',draft='',records={},busy=false}
end

function M.new(session)
  return setmetatable({session=session,contexts={},reviews={},transfers={},after_attach={},info={}},M)
end

function M:remember(current)
  current.kind = current.kind or 'discussion'
  if current.id and not current.context_key then self.contexts[current.id]=current end
  if not self.attached_id and current.id and not current.context_key then self.attached_id=current.id end
end

function M:reset()
  self:close_steps()
  self.selected_identity,self.active_started,self.active_key=nil,nil,nil
  self.archive_view,self.archive_key,self.live_state,self.live_selection=nil,nil,nil,nil
  self.attached_id,self.branch_id,self.discussion_id,self.state = nil,nil,nil,nil
  self.info,self.transfers = {},{}
end

function M:readonly(current)
  if current.loading_implementation or current.implementation_error then return true end
  if self.archive_view then return true end
  if current.archived or current.kind=='step' or current.kind=='validation' or current.kind=='plan' or current.kind=='conclusion' or current.kind=='proposal' then return true end
  if current.kind=='revision' then return not (self.state and self.state.revision_planning) end
  if current.kind=='review' then
    local importing=self.review_actions and self.review_actions.importing
    return (importing and importing.identity==current.step_identity) or current.awaiting_selection
      or current.review_key~=self:review_key(current.step) or not self:eligible(current.step,current.substep)
  end
  if current.preview then return true end
  if current.kind=='planning' then return self.state~=nil or self.info.auto_plan_pending==true or self.info.implementation_execution==true end
  return false
end

function M:label(current)
  local label = ({discussion='Discussion',planning='Implementation planning',review='Review',
    step='Construction',validation='Human validation',plan='Implementation plan',conclusion='Conclusion',proposal='Correction proposal',revision='Revised-plan discussion'})[current.kind] or 'Discussion'
  if current.kind=='review' or current.kind=='step' or current.kind=='validation' then label=label..' · Step '..tostring((current.step or 0)+1) end
  if current.preview then label=label..' · read-only preview' end
  if current.archived then label=label..' · archive' end
  if current.kind=='review' and current.review_key~=self:review_key(current.step) then label=label..' · superseded result' end
  return label
end

local function step_context(current)
  return current.kind=='review' or current.kind=='step' or current.kind=='validation'
end

function M:close_steps()
  local selector=self.selector
  self.selector=nil
  if selector then selector:close() end
end

function M:select(current)
  if not step_context(current) then self:close_steps() end
  self:remember(self.session.current)
  self.session.current=current
  self.session:changed()
end

function M:attached(header)
  self:close_steps()
  local current = self.session.current
  self:remember(current)
  local previous = self.attached_id
  self.attached_id=current.id
  local info = header.info or self.info
  self.info=info
  self.discussion_id = present(info.discussion_id) and info.discussion_id or current.id
  self.branch_id = info.implementation_view and current.id or nil
  current.kind = info.implementation_view and 'planning' or 'discussion'
  current.archived=info.implementation_archive==true
  current.preview=nil
  if previous~=current.id then
    self.state=nil; self.transfers={}; self.selected_identity=nil
    self.archive_view,self.archive_key,self.live_state,self.live_selection=nil,nil,nil,nil
    self.selected_identity,self.active_key,self.active_started=nil,nil,nil
  end
  if self.branch_id then
    current.loading_implementation=info.implementation_execution==true and not info.auto_plan_pending
    current.loading_started=vim.uv.hrtime()/1e6
    current.implementation_error=nil
    local request=self.session:request({type='implementation.state',sessionId=current.id},'implementation.updated',function(event,error)
      if not event then
        current.loading_implementation=nil
        current.implementation_error=error or 'Implementation state is unavailable. Use :SosReconnect to reload.'
        self.session:changed()
      end
    end)
    if not request then
      current.loading_implementation=nil
      current.implementation_error='Implementation state is unavailable. Use :SosReconnect to reload.'
    end
    if self.archive_view and self.selected_identity then self:select_step(self.selected_identity) end
  end
  local action=self.after_attach[header.requestId]
  self.after_attach[header.requestId]=nil
  if action then action() end
end

function M:view(fields, after)
  local session=self.session
  if session.switching then session:notice('Wait for the context transition'); return end
  session.switching=fields.sessionId
  local id=session:request(fields,'session.attached',function(event)
    if not event then session.switching=false; session.transfer=nil; session:changed() end
  end)
  if not id then session.switching=false; return end
  session.attach_request=id
  self.after_attach[id]=after
end

function M:finished()
  if not self.state or self.state.status~='completed' then return false end
  for _, step in ipairs(self.state.steps or {}) do if not step.whole.reviewed then return false end end
  return true
end

function M:discussion_locked()
  return self.branch_id and not self.info.implementation_archive and not self:finished()
end

function M:discussion()
  if self.archive_view then self.session:review_actions():live() end
  local id=self.discussion_id or self.attached_id
  if not id then return end
  if self.branch_id and not self.info.implementation_archive and self:finished() then
    self:view({type='implementation.close',sessionId=id})
  elseif self:discussion_locked() then
    self.session:notice('Discussion is preserved but unavailable until implementation is finished')
  elseif self.attached_id~=id then
    self:view({type='implementation.view',sessionId=id})
  else self:select(self.contexts[id] or self.session.current) end
end

function M:planning()
  if self.branch_id then self:select(self.contexts[self.branch_id]); return end
  local branches=self.info.implementation_branches or {}
  if #branches==0 then self.session:notice('No implementation branch. Use :SosPlan.'); return end
  require('sos.ui').select(branches,{prompt='Implementation branches',format_item=function(row)
    return row.id..(row.frozen and ' · archive' or ' · active')
  end},function(row)
    if row then self:view({type='implementation.view',sessionId=self.discussion_id or self.attached_id,branchId=row.id}) end
  end)
end

function M:eligible(step,substep)
  local state=self.state
  if not state or self.archive_view or self.info.implementation_archive or state.revision_planning or state.status=='revising' then return false end
  local item=(state.steps or {})[(step or -1)+1]
  return item and step==state.review_step and item.whole.complete==true and item.whole.reviewed~=true
    and type(substep)=='number' and substep>=0 and substep<=#item.substeps or false
end

function M:review_key(step)
  local item=self.state and self.state.steps[step+1]
  return item and (self.branch_id..'/review/'..tostring(item.identity)..'/'..tostring(item.generation)) or nil
end

function M:review_context(step,substep)
  local base=self:review_key(step)
  local key=base..'/'..substep..(self.archive_key and '/'..self.archive_key or '')
  local current=self.contexts[key] or context(self.branch_id,'review',self.info.workspace)
  current.context_key,current.review_key,current.step,current.substep=key,base,step,substep
  current.step_identity=self.state.steps[step+1].identity
  current.archived=self.archive_view or self.info.implementation_archive==true
  local saved=self.reviews[base]
  if current.archived then
    if not current.archive_initialized then
      current.records=vim.deepcopy(saved and saved.records or current.records)
      current.archive_initialized=true
    end
    current.busy=false
  elseif saved then current.records,current.busy=saved.records,saved.busy end
  self.contexts[key]=current
  return current
end

function M:review(step,substep,refresh)
  if not self:eligible(step,substep) then self.session:notice('This result is not eligible for review'); return end
  local current=self:review_context(step,substep)
  current.awaiting_selection=nil
  local inspector=self.session.inspector
  local selected=inspector and inspector.visible and inspector.model:selected()
  current.question_context=nil
  local inspected=inspector and inspector.model.step
  local result=self.state.steps[step+1]
  if selected and inspector.model.index==step and inspector.model.part==substep
    and inspected.identity==result.identity and inspected.generation==result.generation
    and inspected.display_revision==result.display_revision then
    current.question_context={path=selected.path,hunk=selected.hunk}
  end
  self:select(current)
  if refresh~=false then self.session:request({type='implementation.state',sessionId=self.branch_id},'implementation.updated') end
end

function M:review_status(current)
  if current.kind~='review' or not self:readonly(current) then return end
  local state=self.state
  local second='Reopen SOS to select a step to review'
  local importing=self.review_actions and self.review_actions.importing
  if importing and importing.identity==current.step_identity then return 'Applying changes\nAwaiting daemon confirmation' end
  if self.archive_view or current.archived then return 'Archived review · Read-only\n'..second end
  if self.session.client.transport.state~='ready' then return 'Disconnected · :SosReconnect\n'..second end
  if current.review_key~=self:review_key(current.step) then return 'Superseded review · Read-only\n'..second end
  if state.status=='running' or state.status=='revising' then
    local phase=state.status=='revising' and 'Preparing' or 'Building'
    local frames={'|','/','-','\\'}
    return phase..' step '..((state.work_step or 0)+1)..' '..frames[math.floor(vim.uv.now()/150)%4+1]..'\n'..second
  end
  if state.status=='interrupted' or state.status=='failed' then return 'Interrupted · :SosContinue to resume\n'..second end
  return 'Select a step to review\n'..second
end

function M:choose_review(step)
  local state=self.state
  local item=state and state.steps[step+1]
  if not item then return end
  local branch=self.branch_id
  local choices={{label='Whole step',index=#item.substeps}}
  for n,part in ipairs(state.plan.steps[step+1].substeps) do
    choices[#choices+1]={label=n..'. '..(type(part)=='table' and part.title or part),index=n-1}
  end
  require('sos.ui').select(choices,{prompt='Review selection',format_item=function(row) return row.label end},function(row)
    if row and branch==self.branch_id and self.state.steps[step+1].identity==item.identity then self:review(step,row.index) end
  end)
end

function M:choose_context()
  local choices={{label=self:discussion_locked() and 'Discussion · Unavailable during implementation' or 'Discussion',action=function() self:discussion() end},
    {label='Implementation planning',action=function() self:planning() end}}
  if self.state then
    local step=self.state.review_step
    local item=self.state.steps[step+1]
    if item and self:eligible(step,#item.substeps) then
      table.insert(choices,{label='Review · Step '..(step+1),action=function() self:choose_review(step) end})
    end
  end
  require('sos.ui').select(choices,{prompt='SOS context',format_item=function(row) return row.label end},function(row)
    if row then row.action() end
  end)
end

function M:send_review(current)
  if self:readonly(current) or current.busy or current.pending then self.session:notice('Review is unavailable or still running'); return end
  local text=current.draft
  if not text:match('%S') then return end
  current.pending=true
  local revision=current.kind=='revision'
  local frame=vim.tbl_extend('force',{type=revision and 'implementation.revision.review' or 'implementation.review',
    sessionId=self.branch_id,step=current.step,substep=current.substep,text=text},current.question_context or {})
  local id=self.session:request(frame,
    revision and 'implementation.revision.review.accepted' or 'implementation.review.accepted',function(event)
      current.pending=nil
      if event then
        current.busy=true
        if current.draft==text then current.draft='' end
      end
      self.session:changed()
    end)
  if not id then current.pending=nil end
  self.session:changed()
end

function M:pace(callback)
  require('sos.ui').select({{label='Step by step',value='step_by_step'},{label='Everything',value='everything'}},
    {prompt='Implementation pace',format_item=function(row) return row.label end},function(row)
      if row then callback(row.value) end
    end)
end

function M:command(name,fields,terminal,callback)
  if self.archive_view and name~='implementation.history' and name~='implementation.state' then self.session:notice('Historical archive is read-only'); return end
  if not self.branch_id then self.session:notice('Open an implementation branch first'); return end
  return self.session:request(vim.tbl_extend('force',{type=name,sessionId=self.branch_id},fields or {}),
    terminal or 'implementation.updated',callback)
end

function M:generate()
  if not self.branch_id or self.info.implementation_archive or self.state then
    self.session:notice('Implementation planning is unavailable in this context'); return
  end
  local principal=self.contexts[self.branch_id]
  if principal.busy or principal.pending then return end
  principal.pending=true
  local id=self.session:request({type='message.send',text='Implementation plan request',mode='sane',implementation_request=true},
    'message.accepted',function(event)
      principal.pending=nil
      if event then principal.busy=true end
      self.session:changed()
    end)
  if not id then principal.pending=nil end
end

function M:open_objective(objective,auto,pace)
  if self.session.current.preview or self.session.current.archived or self.session.current.busy or self.session.switching then
    self.session:notice('Return to an idle Discussion before opening an objective'); return
  end
  local discussion=self.discussion_id or self.attached_id
  if not discussion then self.session:notice('Publish an Objective Plan in a saved Discussion first'); return end
  local existing
  for _, branch in ipairs(self.info.implementation_branches or {}) do
    if branch.objective_id==objective.id then existing=branch; break end
  end
  local fields=existing and {type='implementation.view',sessionId=discussion,branchId=existing.id}
    or {type='implementation.open',sessionId=discussion,objectiveId=objective.id}
  self:view(fields,function()
    if existing or self.info.implementation_archive then return end
    if auto then self:command('implementation.auto_plan',{pace=pace})
    elseif not self.info.implementation_plan_ready and not self.info.implementation_execution then self:generate() end
  end)
end

function M:latest_objective()
  local message
  for n=#self.session.current.records,1,-1 do
    local record=self.session.current.records[n]
    if record.role=='user' or record.role=='assistant' then
      message=record; break
    end
  end
  if not message or message.role~='assistant' then return end
  for _,turn in ipairs(self.info.objective_plan_turns or {}) do
    if turn.message_id==message.id then
      for _,objective in ipairs(self.info.objective_plans or {}) do
        if objective.id==turn.objective_id then return objective,message.id end
      end
    end
  end
end

function M:implement()
  local current=self.session.current
  if self.branch_id and (current.kind=='planning'
    or (current.kind=='plan' and not (current.records[1] or {}).objective_plan)) then
    self:start(); return
  end
  if (current.kind and current.kind~='discussion') or self:readonly(current) then
    self.session:notice('Return to an idle Discussion before implementing an Objective Plan'); return
  end
  if current.busy or current.pending or self.session.switching then
    self.session:notice('Wait for the current turn to finish before implementing an Objective Plan'); return
  end
  if current.history and (current.history.loading or current.history.page.hasNewer) then
    self.session:notice('Return to the latest Discussion message with :SosLatest before implementing'); return
  end
  local objective,message_id=self:latest_objective()
  if not objective then
    self.session:notice('An Objective Plan is required in the last assistant message'); return
  end
  local function unchanged()
    local selected,last_id=self:latest_objective()
    if self.session.current~=current or current.busy or current.pending or self.session.switching
      or (current.kind and current.kind~='discussion')
      or self:readonly(current) or (current.history and (current.history.loading or current.history.page.hasNewer))
      or not selected or selected.id~=objective.id or last_id~=message_id then
      self.session:notice('The Discussion changed. Run :SosImplement again'); return false
    end
    return true
  end
  require('sos.ui').select({'Implementation plan','Implement (Auto plan)'},{prompt='Implement Objective Plan'},function(choice)
    if not choice or not unchanged() then return end
    if choice=='Implementation plan' then self:open_objective(objective,false)
    elseif choice=='Implement (Auto plan)' then
      self:pace(function(pace) if unchanged() then self:open_objective(objective,true,pace) end end)
    end
  end)
end

function M:objectives(auto)
  if self.session.current.preview then self.session:notice('Objective actions are disabled in Discussion preview'); return end
  local versions=self.info.objective_plans or {}
  if #versions==0 then self.session:notice('No Objective Plan has been published'); return end
  require('sos.ui').select(versions,{prompt='Objective Plan version',format_item=function(row)
    local text=require('sos.render').text(row)
    return row.id..' · '..text:gsub('\n',' '):sub(1,70)
  end},function(objective)
    if not objective then return end
    if auto then self:pace(function(pace) self:open_objective(objective,true,pace) end)
    else
      require('sos.ui').select({'Read objective','Open implementation planning'},{prompt='Selected objective'},function(choice)
        if choice=='Read objective' then
          local current=context(self.attached_id,'plan',self.info.workspace)
          current.context_key='objective:'..objective.id
          current.records={vim.tbl_extend('force',objective,{role='assistant',objective_plan=true})}
          self.contexts[current.context_key]=current
          self:select(current)
        elseif choice then self:open_objective(objective,false) end
      end)
    end
  end)
end

function M:start()
  local current,branch=self.session.current,self.branch_id
  if not branch or current.archived or current.preview or self.info.implementation_archive
    or self.archive_view or self.state or self.info.implementation_execution then
    self.session:notice('Implementation is not awaiting plan validation'); return
  end
  if current.busy or current.pending or self.session.switching then
    self.session:notice('Wait for implementation planning to finish'); return
  end
  if not self.info.implementation_plan_ready then
    self.session:notice('A finalized implementation plan is required'); return
  end
  local plan=vim.deepcopy(self.info.implementation_plan)
  self:pace(function(pace)
    if self.session.current~=current or self.branch_id~=branch or self.session.switching
      or current.busy or current.pending or current.archived or current.preview
      or self.state or self.archive_view or self.info.implementation_archive
      or self.info.implementation_execution or not self.info.implementation_plan_ready
      or not vim.deep_equal(plan,self.info.implementation_plan) then
      self.session:notice('Implementation planning changed. Run :SosImplement again'); return
    end
    self:command('implementation.start',{pace=pace})
  end)
end

function M:auto_retry()
  local pace=self.info.auto_plan_pace
  if not self.branch_id or self.info.implementation_archive or (pace~='step_by_step' and pace~='everything') then
    self.session:notice('No saved automatic planning request to retry'); return
  end
  self:command('implementation.auto_plan',{pace=pace})
end

function M:show_plan()
  local state=self.state
  local text=state and state.accepted_plan_markdown
  if not present(text) then
    local plan=self.info.implementation_plan
    text=type(plan)=='table' and require('sos.render').text(plan) or ''
  end
  if not present(text) then self.session:notice('No finalized implementation plan is available'); return end
  local key=(self.branch_id or '')..'/'..(self.archive_key or 'live')..'/plan'
  local current=self.contexts[key] or context(self.branch_id,'plan',self.info.workspace)
  current.context_key=key
  current.archived=self.archive_view or self.info.implementation_archive==true
  current.records={{id=key,role='assistant',content=text}}
  self.contexts[key]=current
  self:select(current)
end

function M:progress(construction)
  local current=self.session.current
  if not construction then
    if self.review_actions and self.review_actions.pending then return 'Review action pending · Awaiting daemon confirmation',false end
    if (current.kind=='review' or current.kind=='revision') and current.busy then
      return 'Review response · Construction: '..tostring((self.state or {}).status or 'idle'),true
    end
    if current.preview then return 'Implementation active · Discussion frozen',false end
  end
  if self.session.client.transport.state~='ready' then return 'Disconnected',false end
  if self.info.auto_plan_pending then
    if self.info.auto_plan_running then return 'Generating implementation plan',true end
    return 'Auto plan stopped · :SosRetryPlan · '..tostring(self.info.auto_plan_error or ''),false
  end
  local state=self.state
  if not state then return (current.busy or current.pending) and 'Generating' or 'Idle',current.busy or current.pending end
  local step=(state.steps or {})[(state.work_step or 0)+1]
  local title=((state.plan or {}).steps or {})[(state.work_step or 0)+1]
  local prefix='Step '..tostring((state.work_step or 0)+1)..'/'..#state.steps
  if construction and (state.status=='running' or state.status=='revising') then
    local total=title and #title.substeps or 0
    local current=math.min((state.work_substep or 0)+1,total)
    local validation=step and step.validation or {}
    local phase=(state.preparing or (step and step.preparing)) and 'Preparing'
      or (validation.started and not validation.complete) and 'Validating' or 'Building'
    return prefix..' · Substep '..current..'/'..total..' · '..phase,true
  end
  if title then prefix=prefix..' · '..title.title end
  if state.status=='running' or state.status=='revising' then
    if state.preparing or (step and step.preparing) then return prefix..' · Preparing',true end
    if step and step.validation.started and not step.validation.complete then return prefix..' · Validating',true end
    local part=title and title.substeps[(state.work_substep or 0)+1]
    local label=type(part)=='table' and part.title or part
    return prefix..' · Building'..(label and ' · '..label or ''),true
  elseif state.status=='interrupted' or state.status=='failed' then return prefix..' · Interrupted · :SosContinue',false
  elseif state.status=='waiting' then return 'Waiting for review · Step '..tostring((state.review_step or 0)+1),false
  elseif state.status=='completed' then
    return self:finished() and 'Completed · All steps validated' or 'Construction completed · Waiting for human review',false
  elseif state.status=='archived' then return 'Archived implementation',false end
  return tostring(state.status),false
end

function M:step_label(index)
  local state=self.state
  local step=state.steps[index+1]
  if step.preparing or (state.preparing and index==state.work_step) then return 'Preparing' end
  if step.whole.reviewed then return 'Validated' end
  if step.whole.complete then return index==state.review_step and 'Ready for review' or 'Built · Review not yet available' end
  if index==state.work_step and (state.status=='interrupted' or state.status=='failed') then return 'Interrupted' end
  if index==state.work_step and state.status=='running' then
    return step.validation.started and not step.validation.complete and 'Validating' or 'Building'
  end
  return 'Pending'
end

function M:step_summary(current,index)
  self.session:history(current):install(require('sos.construction').records(self,current,index,current.kind=='validation'),{})
end

function M:observe_execution(state)
  state=state or self.state
  if not state or self.info.implementation_archive or state.status~='running' or state.preparing then return end
  local step=state.steps[(state.work_step or 0)+1]
  if not step then return end
  local key=table.concat({self.branch_id,step.identity,tostring(step.display_revision or 0),tostring(state.work_substep or 0)},'/')
  local now=vim.uv.hrtime()/1e6
  if type(state.active_duration_ms)=='number' then self.active_started=now-state.active_duration_ms
  elseif key~=self.active_key then self.active_started=now end
  self.active_key=key
end

function M:human_validation()
  local index=self.session.current.step
  local step=index and self.state and self.state.steps[index+1]
  if not step or not self:eligible(index,#step.substeps) then self.session:notice('Select an eligible completed step first'); return end
  if self.session.inspector and self.session.inspector.visible then self.session.inspector:hide() end
  local key=self.branch_id..'/validation/'..step.identity
  local current=self.contexts[key] or context(self.branch_id,'validation',self.info.workspace)
  current.context_key,current.step=key,index
  self.contexts[key]=current
  self:step_summary(current,index)
  self:select(current)
end

function M:inspect_part(identity,part)
  local step
  for _,item in ipairs(self.state.steps) do if item.identity==identity then step=item; break end end
  if not step or not step.whole.complete then return end
  self:select_step(identity)
  self.session:inspect()
  self.session.inspector.model.part=part
  self.session.inspector.model:refresh()
  self.session.inspector:render()
end

function M:select_step(identity,awaiting_selection)
  for n,item in ipairs((self.state or {}).steps or {}) do
    if item.identity==identity then
      self.selected_identity=identity
      local current=self:review_context(n-1,#item.substeps)
      current.awaiting_selection=awaiting_selection or nil
      current.building_review=not item.whole.complete
      self:select(current)
      if self.selector then self.selector:update(self:step_rows(self.selector.origin),identity) end
      if self.on_select then self.on_select(n-1,item) end
      return
    end
  end
end

function M:conclusion()
  local text=self:conclusion_text()
  if not present(text) then self.session:notice('No conclusion is available yet'); return end
  local key=self.branch_id..'/'..(self.archive_key or 'live')..'/conclusion'
  local current=self.contexts[key] or context(self.branch_id,'conclusion',self.info.workspace)
  current.context_key,current.archived=key,self.archive_view or self.info.implementation_archive==true
  current.records={{id=key,role='assistant',content=text}}
  if self:finished() and not current.archived then
    current.records[#current.records+1]={id=key..'/close',role='construction',content='\n[ Close implementation ]',group='SosAction',control='close_implementation'}
    current.focus_control='close_implementation'
  end
  self.contexts[key]=current
  self:select(current)
end

function M:conclusion_text()
  local steps=(self.state or {}).steps or {}
  for _,step in ipairs(steps) do if not step.whole.complete then return end end
  return self.state and (self.state.conclusion or (steps[#steps] or {}).conclusion)
end

function M:step_rows(origin)
  local rows={}
  for n,item in ipairs((self.state or {}).steps or {}) do
    local identity=item.identity
    local group=item.whole.reviewed and 'SosValidated' or item.whole.complete and 'SosNormal'
      or n-1==self.state.work_step and 'SosActivity' or 'SosMuted'
    local plan=self.state.plan.steps[n]
    local preview={}
    for part,definition in ipairs(plan.substeps) do
      local result=item.substeps[part] or {}
      local explanation=result.unnecessary and 'No longer necessary given the changes in previous steps.' or result.explanation
      preview[#preview+1]='## '..part..'. '..(type(definition)=='table' and definition.title or definition)
      preview[#preview+1]=explanation and explanation:match('%S') and explanation or 'No explanation available for this substep.'
      preview[#preview+1]=''
    end
    rows[#rows+1]={key=identity,title=plan.title,label=n..'. '..plan.title..' · '..self:step_label(n-1),group=group,
      preview=vim.split(table.concat(preview,'\n'),'\n',{plain=true}),
      choose=function()
        self.session:inspect(true,origin,identity)
      end}
  end
  return rows
end

function M:steps(automatic)
  local origin
  if automatic then
    local panel=self.session.panel
    if not self.branch_id or not self.state or not self.state.plan or self:finished()
      or not step_context(self.session.current) then return end
    local win=panel and (panel.input or panel.transcript)
    if not win or not vim.api.nvim_win_is_valid(win) then return end
    origin={window=win,buffer=vim.api.nvim_win_get_buf(win),from_chat=true,
      insert=vim.api.nvim_get_current_win()==win and vim.api.nvim_get_mode().mode:sub(1,1)=='i'}
  end
  if not self.branch_id then self.session:notice('Open an implementation first'); return end
  if self.selector and self.selector:visible() then vim.api.nvim_set_current_win(self.selector.window); return end
  origin=origin or self.session:inspection_origin()
  self.selector=require('sos.selector').open('Implementation steps',self:step_rows(origin),self.selected_identity,origin)
  if not automatic then self:command('implementation.state') end
end

function M:sync_navigation()
  if not self.selector or not self.selector:visible() then return end
  if not self.state or not self.state.plan or not self.branch_id then
    if self.selector then self.selector:close(); self.selector=nil end
    return
  end
  self.selector:update(self:step_rows(self.selector.origin))
end

function M:cancel_construction()
  local branch=self.branch_id
  require('sos.ui').select({'Keep building','Cancel construction'},{prompt='Interrupt implementation?'},function(choice)
    if choice=='Cancel construction' and branch==self.branch_id then
      self.session:request({type='execution.cancel'},'execution.cancel_requested')
    end
  end)
end

function M:resume()
  if not self.state or (self.state.status~='interrupted' and self.state.status~='failed') or self.info.implementation_archive then
    self.session:notice('No resumable construction is selected'); return
  end
  local branch=self.branch_id
  require('sos.ui').select({'Keep interrupted','Resume construction'},{prompt='Resume implementation?'},function(choice)
    if choice=='Resume construction' and branch==self.branch_id then self:command('implementation.resume') end
  end)
end

function M:review_event(event)
  if event.type=='btw.started' and event.branch and event.branch.revision_planning then
    if event.sessionId~=self.branch_id then return true end
    self.transfers[event.requestId]={transfer=History.transfer(event),branch=event.branch,key=self.branch_id..'/revision'}
    return true
  elseif event.type=='btw.started' and event.branch and event.branch.review then
    local step=event.branch.review_step
    if event.sessionId~=self.branch_id or not self.state or not self.state.steps[step+1] then return true end
    self.transfers[event.requestId]={transfer=History.transfer(event),branch=event.branch,key=self:review_key(step)}
    return true
  end
  local transfer=self.transfers[event.requestId]
  if transfer and (event.type=='btw.chunk' or event.type=='btw.finished') then
    local converted=vim.tbl_extend('force',event,{type=event.type=='btw.chunk' and 'session.history_chunk' or 'session.attached'})
    local ok,records=pcall(History.accept,transfer.transfer,converted)
    if not ok then self.session.client.transport:stop('Invalid review history: '..records); return true end
    if records then
      self.transfers[event.requestId]=nil
      if transfer.branch.revision_planning then
        local current=self.contexts[transfer.key] or {id=self.branch_id,context_key=transfer.key,kind='revision',mode='sane',draft='',workspace=self.info.workspace}
        current.records=records; current.busy=transfer.branch.status=='running'
        self.contexts[transfer.key]=current
        self.session:notice(transfer.branch.error,nil,current)
        return true
      end
      if transfer.key==self:review_key(transfer.branch.review_step) then
        local saved={records=records,busy=transfer.branch.status=='running'}
        self.reviews[transfer.key]=saved
        for _, current in pairs(self.contexts) do
          if current.review_key==transfer.key and not current.archived then
            current.records,current.busy=saved.records,saved.busy
            self.session:notice(transfer.branch.error,nil,current)
          end
        end
      end
    end
    return true
  end
  return false
end

function M:event(event)
  if event.info then self.info=event.info end
  if event.type=='connection.closed' then
    self.transfers={}
    for _, current in pairs(self.contexts) do
      current.busy,current.pending=false,nil
      if current.loading_implementation then
        current.loading_implementation=nil
        current.implementation_error='Disconnected. Use :SosReconnect to load implementation.'
      end
      if current.history then current.history:finish(); current.history:invalidate() end
      for _,record in ipairs(current.records or {}) do
        if record._started_at then record.duration_ms=math.max(0,vim.uv.hrtime()/1e6-record._started_at); record._started_at=nil end
      end
    end
  elseif event.type=='session.created' then
    self.attached_id=event.session.id
    self.discussion_id=event.session.id
  elseif event.type=='session.reset' then self:reset()
  elseif event.type=='implementation.updated' and event.sessionId==self.branch_id then
    if self.archive_view then
      self.live_state=type(event.implementation)=='table' and event.implementation or nil
      self:observe_execution(self.live_state)
      return true
    end
    local initial=self.state==nil
    local previous=self.state
    self.state=type(event.implementation)=='table' and event.implementation or nil
    local principal=self.contexts[self.branch_id]
    local opening=principal and principal.loading_implementation
    if principal then principal.loading_implementation,principal.implementation_error=nil,nil end
    if opening and not self.state then principal.implementation_error='Implementation state is unavailable. Use :SosReconnect to reload.' end
    self:observe_execution()
    if self.state then
      local selected=false
      for _,step in ipairs(self.state.steps) do if step.identity==self.selected_identity then selected=true; break end end
      if not selected then
        local index=(self.state.status=='interrupted' or self.state.status=='failed') and self.state.work_step or 0
        self.selected_identity=(self.state.steps[(index or 0)+1] or {}).identity
      end
      local kind=self.session.current.kind
      local entering=(initial or opening) and (kind=='planning' or kind=='plan') and self.selected_identity
      if entering then self:select_step(self.selected_identity,true) end
      local target=self.after_import
      self.after_import=nil
      if target and self.state.steps[target+1] then
        if self.session.inspector and self.session.inspector.visible then self.session.inspector:hide() end
        if self:finished() then self:conclusion()
        else self:select_step(self.state.steps[math.min(target+2,#self.state.steps)].identity,true) end
      elseif not self.archive_view and ((self.state.status=='revising' and (not previous or previous.status~='revising'))
        or (previous and previous.revision_planning and not self.state.revision_planning)) then
        if self.session.inspector and self.session.inspector.visible then self.session.inspector:hide() end
        self:select_step(self.state.steps[(self.state.review_step or 0)+1].identity,true)
      end
      local current=self.session.current
      if current.kind=='review' and current.building_review and current.review_key~=self:review_key(current.step) then
        self:select_step(self.selected_identity,current.awaiting_selection)
        current=self.session.current
      end
      if current.kind=='review' and current.building_review and self.state.steps[current.step+1].whole.complete then
        current.building_review=nil
        local inspector=self.session.inspector
        if not self:readonly(current) and inspector and inspector.visible and inspector.model.index==current.step then
          self:review(current.step,inspector.model.part,false)
          current=self.session.current
        end
      end
      if current.kind=='review' and current.review_key~=self:review_key(current.step) then
        current.question_context=nil
        self.session:notice('This review belongs to a superseded result. Select the corrected step and use :SosReview. Your draft is retained.','info',current)
      end
      if current.kind=='step' or current.kind=='validation' then
        for n,item in ipairs(self.state.steps) do
          if item.identity==self.selected_identity then current.step=n-1; self:step_summary(current,n-1); break end
        end
      end
      if entering and not self.state.revision_planning then self:steps(true) end
    end
    self:sync_navigation()
    if self.state and self.state.revision_planning and (initial or not previous.revision_planning)
      and self.session.current.kind~='revision' then
      self.session:review_actions():revision()
    elseif self.state and type(self.state.proposed_rework)=='string' and self.state.proposed_rework~='' then
      self.session:notice('Correction proposal available · :SosProposal','info')
      if self.session.current.kind=='review' and (not previous or previous.proposed_rework~=self.state.proposed_rework) then
        self.session.current.focus_control='accept_proposal'
      end
    end
    return true
  elseif self:review_event(event) then return true
  elseif event.sessionId==self.branch_id and self.branch_id then
    local principal=self.contexts[self.branch_id]
    if self.state or self.info.auto_plan_pending then
      if principal and (event.type=='execution.completed' or event.type=='execution.failed' or event.type=='execution.cancelled') then
        self.session:restore_queue(principal)
      end
      return event.messages~=nil or event.type:match('^execution%.')~=nil
    end
    if principal and principal~=self.session.current then
      if event.messages or event.type:match('^execution%.') then self.session:history(principal):live(event) end
      if event.type=='execution.completed' or event.type=='execution.failed' or event.type=='execution.cancelled' then
        principal.busy=false
        self.session:restore_queue(principal)
      end
      return event.messages~=nil or event.type:match('^execution%.')~=nil
    end
  end
  return false
end

return M
