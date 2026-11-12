local M={}
M.__index=M
function M.new(workflow) return setmetatable({workflow=workflow},M) end
function M:selection()
  local w=self.workflow
  local inspector=w.session.inspector
  if inspector and inspector.visible then return inspector.model.index,inspector.model.part,inspector.model:selected() end
  local current=w.session.current
  if current.kind=='review' then return current.step,current.substep end
  for n,step in ipairs((w.state or {}).steps or {}) do
    if step.identity==w.selected_identity then return n-1,#step.substeps end
  end
end
function M:available(step,part)
  local w=self.workflow
  return not self.pending and not self.confirming and not w.archive_view and w:eligible(step,part)
    and not (type(w.state.proposed_rework)=='string' and w.state.proposed_rework~='')
end
function M:snapshot(step)
  local w=self.workflow
  return {branch=w.branch_id,revision=w.state.revision or 0,identity=w.state.steps[step+1].identity,step=step}
end
function M:current(token)
  local w=self.workflow
  return not w.archive_view and token.branch==w.branch_id and w.state
    and token.revision==(w.state.revision or 0) and w.state.steps[token.step+1]
    and w.state.steps[token.step+1].identity==token.identity
end
function M:submit(kind,fields,token)
  local w=self.workflow
  if self.pending or not self:current(token) then w.session:notice('Result changed or another review action is pending'); return end
  local lock={}
  self.pending=lock
  local id=w:command(kind,vim.tbl_extend('force',fields,{revision=token.revision}),'implementation.updated',function()
    if self.pending==lock then self.pending=nil end
    w.session:changed()
  end)
  if not id then self.pending=nil end
  w.session:changed()
  return id
end
function M:indicator()
  local w=self.workflow
  local step,part=self:selection()
  if not self:available(step,part) then w.session:notice('Select an eligible completed result first'); return end
  local item=w.state.steps[step+1]
  if part>=#item.substeps then w.session:notice('Whole step is not an indicator; use :SosValidate for final import'); return end
  return self:submit(item.substeps[part+1].reviewed and 'implementation.unvalidate' or 'implementation.validate',
    {step=step,substep=part},self:snapshot(step))
end
function M:validate()
  local w=self.workflow
  local step,part=self:selection()
  if not self:available(step,part) then w.session:notice('Select an eligible completed result first'); return end
  local item=w.state.steps[step+1]
  local unchecked=0
  for _,substep in ipairs(item.substeps) do if not substep.reviewed then unchecked=unchecked+1 end end
  local token=self:snapshot(step)
  self.confirming=true
  vim.ui.select({'Keep reviewing','Validate and import step'},
    {prompt='Import step '..(step+1)..' into original workspace? '..unchecked..' unchecked indicator(s) will be accepted.'},function(choice)
      self.confirming=false
      if choice=='Validate and import step' then self:submit('implementation.validate',{step=step,substep=#item.substeps},token) end
    end)
end

function M:proposal()
  local w=self.workflow
  if not w.state then w.session:notice('No implementation result'); return end
  local state=w.state
  local text=state.proposed_rework or ''
  if text=='' then w.session:notice('No correction proposal is pending'); return end
  local current={id=w.branch_id,context_key=w.branch_id..'/proposal/'..tostring(state.revision),kind='proposal',
    archived=w.archive_view or w.info.implementation_archive,mode='sane',draft='',workspace=w.info.workspace,
    records={{id='proposal',role='assistant',content=text..'\n\n'..(state.proposed_plan_markdown or '')}}}
  w.contexts[current.context_key]=current; w:select(current)
  w.session:notice(state.revision_planning and 'Candidate plan · :SosRevision · :SosAcceptProposal to execute · :SosRejectProposal'
    or 'Proposal · :SosAcceptProposal / :SosRejectProposal')
end

function M:proposal_action(accept)
  local w=self.workflow
  local state=w.state
  if not state or w.archive_view or w.info.implementation_archive or self.pending or self.confirming
    or type(state.proposed_rework)~='string' or state.proposed_rework=='' then w.session:notice('No actionable proposal'); return end
  for _,context in pairs(w.contexts) do
    if (context.kind=='review' or context.kind=='revision') and (context.busy or context.pending) then
      w.session:notice('Wait for the review or plan response'); return
    end
  end
  local token=self:snapshot(state.review_step)
  local kind=not accept and 'implementation.rework.reject'
    or state.revision_planning and 'implementation.rework'
    or type(state.proposed_plan_markdown)=='string' and state.proposed_plan_markdown~='' and 'implementation.revision.open'
    or 'implementation.rework'
  local label=kind=='implementation.revision.open' and 'Open revised-plan discussion (no execution)'
    or accept and 'Accept proposal and execute corrections' or 'Reject proposal and resume paused work'
  self.confirming=true
  vim.ui.select({'Keep reviewing',label},{prompt='Revision '..token.revision..': '..label..'?'},function(choice)
    self.confirming=false
    if choice==label then self:submit(kind,{},token) end
  end)
end

function M:revision()
  local w=self.workflow
  if not w.state or not w.state.revision_planning or w.archive_view or w.info.implementation_archive then
    w.session:notice('No active revised-plan discussion'); return
  end
  local key=w.branch_id..'/revision'
  local current=w.contexts[key] or {id=w.branch_id,context_key=key,kind='revision',mode='sane',draft='',records={},workspace=w.info.workspace}
  w.contexts[key]=current; w:select(current)
  w:command('implementation.state')
end

function M:versions()
  local w=self.workflow
  local branch=w.branch_id
  w:command('implementation.history',{all=true,step=0},'implementation.history',function(event)
    if not event or branch~=w.branch_id then return end
    local rows={{label='Current implementation'}}
    for n,state in ipairs(event.implementations or {}) do
      rows[#rows+1]={label='Archive '..n..' · '..(state.version_request or ''),state=state,key='archive/'..n}
    end
    vim.ui.select(rows,{prompt='Preserved implementation versions',format_item=function(row) return row.label end},function(row)
      if not row or branch~=w.branch_id then return end
      if row.state then
        if not w.archive_view then w.live_state,w.live_selection=w.state,w.selected_identity end
        w.archive_view,w.archive_key=true,row.key
        w.state=row.state
        w.selected_identity=(row.state.steps[1] or {}).identity
      else self:live() end
      if w.selected_identity then w:select_step(w.selected_identity); w.session:inspect() end
    end)
  end)
end

function M:live()
  local w=self.workflow
  if not w.archive_view then return end
  w.state,w.selected_identity=w.live_state,w.live_selection
  w.archive_view,w.archive_key,w.live_state,w.live_selection=nil,nil,nil,nil
end
return M
