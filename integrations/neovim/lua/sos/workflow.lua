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
  self.archive_view,self.archive_key,self.live_state,self.live_selection=nil,nil,nil,nil
  self.attached_id,self.branch_id,self.discussion_id,self.state = nil,nil,nil,nil
  self.info,self.transfers = {},{}
end

function M:readonly(current)
  if self.archive_view then return true end
  if current.archived or current.kind=='step' or current.kind=='plan' or current.kind=='conclusion' or current.kind=='proposal' then return true end
  if current.kind=='revision' then return not (self.state and self.state.revision_planning) end
  if current.kind=='review' then return not self:eligible(current.step,current.substep) end
  if current.preview then return true end
  if current.kind=='planning' then return self.state~=nil or self.info.auto_plan_pending==true end
  return false
end

function M:label(current)
  local label = ({discussion='Discussion',planning='Implementation planning',review='Review',
    step='Step inspection',plan='Implementation plan',conclusion='Conclusion',proposal='Correction proposal',revision='Revised-plan discussion'})[current.kind] or 'Discussion'
  if current.kind=='review' or current.kind=='step' then label=label..' · Step '..tostring((current.step or 0)+1) end
  if current.preview then label=label..' · read-only preview' end
  if current.archived then label=label..' · archive' end
  return label
end

function M:select(current)
  self:remember(self.session.current)
  self.session.current=current
  self.session:changed()
end

function M:attached(header)
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
  end
  if self.branch_id then
    self.session:request({type='implementation.state',sessionId=current.id},'implementation.updated')
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

function M:discussion()
  if self.archive_view then self.session:review_actions():live() end
  local id=self.discussion_id or self.attached_id
  if not id then return end
  if self.branch_id and not self.info.implementation_archive and self:finished() then
    self:view({type='implementation.close',sessionId=id})
  elseif self.branch_id and (self.state or self.info.auto_plan_pending or (self.contexts[self.attached_id] or {}).busy) then
    local current=self.contexts[id] or context(id,'discussion',self.info.workspace)
    current.preview=true
    self.contexts[id]=current
    self:select(current)
    if #current.records==0 then self.session:history(current):navigate('latest') end
  elseif self.attached_id~=id then
    self:view({type='implementation.view',sessionId=id})
  else self:select(self.contexts[id] or self.session.current) end
end

function M:planning()
  if self.branch_id then self:select(self.contexts[self.branch_id]); return end
  local branches=self.info.implementation_branches or {}
  if #branches==0 then self.session:notice('No implementation branch. Use :SosPlan.'); return end
  vim.ui.select(branches,{prompt='Implementation branches',format_item=function(row)
    return row.id..(row.frozen and ' · archive' or ' · active')
  end},function(row)
    if row then self:view({type='implementation.view',sessionId=self.discussion_id or self.attached_id,branchId=row.id}) end
  end)
end

function M:eligible(step,substep)
  local state=self.state
  if not state or self.archive_view or self.info.implementation_archive or state.revision_planning then return false end
  local item=(state.steps or {})[(step or -1)+1]
  return item and step==state.review_step and item.whole.complete==true and item.whole.reviewed~=true
    and type(substep)=='number' and substep>=0 and substep<=#item.substeps or false
end

function M:review_key(step)
  local item=self.state and self.state.steps[step+1]
  return item and (self.branch_id..'/review/'..tostring(item.identity)..'/'..tostring(item.generation)) or nil
end

function M:review(step,substep)
  if not self:eligible(step,substep) then self.session:notice('This result is not eligible for review'); return end
  local base=self:review_key(step)
  local key=base..'/'..substep
  local current=self.contexts[key] or context(self.branch_id,'review',self.info.workspace)
  current.context_key,current.review_key,current.step,current.substep=key,base,step,substep
  local inspector=self.session.inspector
  local selected=inspector and inspector.visible and inspector.model:selected()
  current.question_context=nil
  if selected and inspector.model.index==step and inspector.model.part==substep then
    current.question_context={path=selected.path,hunk=selected.hunk}
  end
  local saved=self.reviews[base]
  if saved then current.records,current.busy=saved.records,saved.busy end
  self.contexts[key]=current
  self:select(current)
  self.session:request({type='implementation.state',sessionId=self.branch_id},'implementation.updated')
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
  vim.ui.select(choices,{prompt='Review selection',format_item=function(row) return row.label end},function(row)
    if row and branch==self.branch_id and self.state.steps[step+1].identity==item.identity then self:review(step,row.index) end
  end)
end

function M:choose_context()
  local choices={{label='Discussion',action=function() self:discussion() end},
    {label='Implementation planning',action=function() self:planning() end}}
  if self.state then
    local step=self.state.review_step
    local item=self.state.steps[step+1]
    if item and self:eligible(step,#item.substeps) then
      table.insert(choices,{label='Review · Step '..(step+1),action=function() self:choose_review(step) end})
    end
  end
  vim.ui.select(choices,{prompt='SOS context',format_item=function(row) return row.label end},function(row)
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
  vim.ui.select({{label='Step by step',value='step_by_step'},{label='Everything',value='everything'}},
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
    if self.info.implementation_archive then return end
    if auto then self:command('implementation.auto_plan',{pace=pace})
    elseif not self.info.implementation_plan_ready and not self.info.implementation_execution then self:generate() end
  end)
end

function M:objectives(auto)
  if self.session.current.preview then self.session:notice('Objective actions are disabled in Discussion preview'); return end
  local versions=self.info.objective_plans or {}
  if #versions==0 then self.session:notice('No Objective Plan has been published'); return end
  vim.ui.select(versions,{prompt='Objective Plan version',format_item=function(row)
    local text=require('sos.render').text(row)
    return row.id..' · '..text:gsub('\n',' '):sub(1,70)
  end},function(objective)
    if not objective then return end
    if auto then self:pace(function(pace) self:open_objective(objective,true,pace) end)
    else
      vim.ui.select({'Read objective','Open implementation planning'},{prompt='Selected objective'},function(choice)
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
  if self.info.implementation_archive or not self.info.implementation_plan_ready then
    self.session:notice('A finalized implementation plan is required'); return
  end
  self:pace(function(pace) self:command('implementation.start',{pace=pace}) end)
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

function M:progress()
  local current=self.session.current
  if self.review_actions and self.review_actions.pending then return 'Review action pending · Awaiting daemon confirmation',false end
  if (current.kind=='review' or current.kind=='revision') and current.busy then
    return 'Review response · Construction: '..tostring((self.state or {}).status or 'idle'),true
  end
  if current.preview then return 'Implementation active · Discussion frozen',false end
  if self.session.client.transport.state~='ready' then return 'Disconnected',false end
  if self.info.auto_plan_pending then
    if self.info.auto_plan_running then return 'Generating implementation plan',true end
    return 'Auto plan stopped · :SosRetryPlan · '..tostring(self.info.auto_plan_error or ''),false
  end
  local state=self.state
  if not state then return current.busy and 'Generating' or current.pending and 'Sending' or 'Idle',current.busy or current.pending end
  local step=(state.steps or {})[(state.work_step or 0)+1]
  local title=((state.plan or {}).steps or {})[(state.work_step or 0)+1]
  local prefix='Step '..tostring((state.work_step or 0)+1)..'/'..#state.steps
  if title then prefix=prefix..' · '..title.title end
  if state.status=='running' then
    if state.preparing or (step and step.preparing) then return prefix..' · Preparing',true end
    if step and step.validation.started and not step.validation.complete then return prefix..' · Validating',true end
    return prefix..' · Building',true
  elseif state.status=='interrupted' then return prefix..' · Interrupted · :SosContinue',false
  elseif state.status=='waiting' then return 'Waiting for review · Step '..tostring((state.review_step or 0)+1),false
  elseif state.status=='completed' then
    return self:finished() and 'Completed · All steps validated' or 'Construction completed · Waiting for human review',false
  elseif state.status=='archived' then return 'Archived implementation',false end
  return tostring(state.status),false
end

function M:step_label(index)
  local state=self.state
  local step=state.steps[index+1]
  if step.whole.reviewed then return 'Validated' end
  if step.whole.complete then return index==state.review_step and 'Ready for review' or 'Built · Review not yet available' end
  if step.preparing or (state.preparing and index==state.work_step) then return 'Preparing' end
  if index==state.work_step and state.status=='interrupted' then return 'Interrupted' end
  if index==state.work_step and state.status=='running' then
    return step.validation.started and not step.validation.complete and 'Validating' or 'Building'
  end
  return 'Pending'
end

function M:step_summary(current,index)
  local item=self.state.steps[index+1]
  local planned=self.state.plan.steps[index+1]
  local lines={planned.title,self:step_label(index),planned.objective or '',item.whole.explanation or ''}
  for n,title in ipairs(planned.substeps or {}) do
    lines[#lines+1]=n..'. '..(type(title)=='table' and title.title or title)
    lines[#lines+1]=(item.substeps[n] or {}).explanation or ''
  end
  if self:eligible(index,#item.substeps) then lines[#lines+1]='Use :SosContext to open this step review.' end
  current.records={{id=current.context_key,role='assistant',content=table.concat(lines,'\n')}}
end

function M:select_step(identity)
  for n,item in ipairs((self.state or {}).steps or {}) do
    if item.identity==identity then
      self.selected_identity=identity
      local key=self.branch_id..'/'..(self.archive_key or 'live')..'/step/'..identity
      local current=self.contexts[key] or context(self.branch_id,'step',self.info.workspace)
      current.context_key,current.step,current.archived=key,n-1,self.archive_view or self.info.implementation_archive==true
      self.contexts[key]=current
      self:step_summary(current,n-1)
      self:select(current)
      if self.on_select then self.on_select(n-1,item) end
      return
    end
  end
end

function M:conclusion()
  local steps=(self.state or {}).steps or {}
  local text=(steps[#steps] or {}).conclusion
  if not present(text) then self.session:notice('No conclusion is available yet'); return end
  local key=self.branch_id..'/'..(self.archive_key or 'live')..'/conclusion'
  local current=self.contexts[key] or context(self.branch_id,'conclusion',self.info.workspace)
  current.context_key,current.archived=key,self.archive_view or self.info.implementation_archive==true
  current.records={{id=key,role='assistant',content=text}}
  self.contexts[key]=current
  self:select(current)
end

function M:step_rows()
  local rows={{key='plan',label='Implementation plan',choose=function() self:show_plan() end}}
  for n,item in ipairs((self.state or {}).steps or {}) do
    local identity=item.identity
    rows[#rows+1]={key=identity,label=n..'. '..self.state.plan.steps[n].title..' · '..self:step_label(n-1),
      choose=function() self:select_step(identity); self.session:inspect() end}
  end
  local steps=(self.state or {}).steps or {}
  if present((steps[#steps] or {}).conclusion) then
    rows[#rows+1]={key='conclusion',label='Conclusion',choose=function() self:conclusion() end}
  end
  return rows
end

function M:steps()
  if not self.branch_id then self.session:notice('Open an implementation first'); return end
  self.selector=require('sos.selector').open('Implementation steps',self:step_rows())
  self:command('implementation.state')
end

function M:cancel_construction()
  local branch=self.branch_id
  vim.ui.select({'Keep building','Cancel construction'},{prompt='Interrupt implementation?'},function(choice)
    if choice=='Cancel construction' and branch==self.branch_id then
      self.session:request({type='execution.cancel'},'execution.cancel_requested')
    end
  end)
end

function M:resume()
  if not self.state or self.state.status~='interrupted' or self.info.implementation_archive then
    self.session:notice('No resumable construction is selected'); return
  end
  local branch=self.branch_id
  vim.ui.select({'Keep interrupted','Resume construction'},{prompt='Resume implementation?'},function(choice)
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
        if transfer.branch.error then current.notice=transfer.branch.error end
        return true
      end
      if transfer.key==self:review_key(transfer.branch.review_step) then
        local saved={records=records,busy=transfer.branch.status=='running'}
        self.reviews[transfer.key]=saved
        for _, current in pairs(self.contexts) do
          if current.review_key==transfer.key then
            current.records,current.busy=saved.records,saved.busy
            if transfer.branch.error then current.notice=transfer.branch.error end
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
      if current.history then current.history:invalidate() end
    end
  elseif event.type=='session.created' then
    self.attached_id=event.session.id
    self.discussion_id=event.session.id
  elseif event.type=='session.reset' then self:reset()
  elseif event.type=='implementation.updated' and event.sessionId==self.branch_id then
    if self.archive_view then self.live_state=type(event.implementation)=='table' and event.implementation or nil; return true end
    local initial=self.state==nil
    self.state=type(event.implementation)=='table' and event.implementation or nil
    if self.state then
      if not self.selected_identity and self.state.steps[1] then self.selected_identity=self.state.steps[1].identity end
      if initial and self.session.current.kind=='planning' and self.selected_identity then self:select_step(self.selected_identity) end
      local current=self.session.current
      if current.kind=='step' then
        for n,item in ipairs(self.state.steps) do
          if item.identity==self.selected_identity then current.step=n-1; self:step_summary(current,n-1); break end
        end
      end
    end
    if self.selector then self.selector:update(self:step_rows()) end
    if self.state and self.state.revision_planning and self.session.current.kind~='revision' then
      self.session:review_actions():revision()
    elseif self.state and type(self.state.proposed_rework)=='string' and self.state.proposed_rework~='' then
      self.session.current.notice='Correction proposal available · :SosProposal'
    end
    return true
  elseif self:review_event(event) then return true
  elseif event.sessionId==self.branch_id and self.branch_id then
    local principal=self.contexts[self.branch_id]
    if self.state or self.info.auto_plan_pending then return event.messages~=nil or event.type:match('^execution%.')~=nil end
    if principal and principal~=self.session.current then
      if event.messages then self.session:history(principal):live(event) end
      if event.type=='execution.completed' or event.type=='execution.failed' or event.type=='execution.cancelled' then principal.busy=false end
      return event.messages~=nil or event.type:match('^execution%.')~=nil
    end
  end
  return false
end

return M
