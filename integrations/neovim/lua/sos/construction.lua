-- Display-only construction timeline. Evidence comes exclusively from daemon state.
local M={}
local internal={report_implementation_progress=true,complete_implementation_step=true}
function M.progress(step)
  local total,done=#step.substeps,0
  for _,part in ipairs(step.substeps) do
    if part.finalized or part.unnecessary or part.complete then done=done+1 end
  end
  if (step.validation or {}).started or step.whole.complete then done=total end
  return done,total
end
function M.statistics(part)
  if not part.complete or type(part.duration_ms)~='number' then return '' end
  local seconds=math.floor(part.duration_ms/1000)
  local text=seconds<60 and seconds..'s' or string.format('%dm %02ds',math.floor(seconds/60),seconds%60)
  local stats=part.model_statistics or {}
  if type(stats.output_tokens)=='number' and type(stats.duration_ms)=='number' and stats.duration_ms>0 then
    text=text..' | '..string.format('%.1f',stats.output_tokens*1000/stats.duration_ms):gsub('%.0$','')..' tok/s'
  end
  return text
end
local function activity(part,key,running,started,active)
  local groups,calls={},{}
  local function group()
    if not groups[#groups] or groups[#groups].label=='Compaction' then
      groups[#groups+1]={id=key..'/activity/'..(#groups+1),role='activity',activities={},count=0}
    end
    return groups[#groups]
  end
  for _,message in ipairs(part.activity or {}) do
    local checkpoint=message.compaction~=nil or (message.id or ''):match('^compaction%-status%-')
    if checkpoint then
      local text=require('sos.render').text(message)
      local current=groups[#groups]
      if not current or current.label~='Compaction' or text:match('^started') then
        current={id=key..'/compaction/'..(#groups+1),role='activity',label='Compaction',activities={},count=0}
        groups[#groups+1]=current
      end
      current.activities={{live=true,activityIndex=0,kind='reasoning',title='Summary',text=text}}
      current.count=1
      current.compacting=message.compaction==nil and text:match('^started')~=nil
    else
      for _,block in ipairs(type(message.content)=='table' and message.content or {}) do
        if block.type=='tool_result' then
          if calls[block.callId] then calls[block.callId].result=block end
        elseif (block.type=='tool_call' and not internal[block.toolName])
          or ((block.type=='text' or block.type=='reasoning') and (block.text or ''):match('%S')) then
          local current=group()
          local item={live=true,activityIndex=#current.activities,kind=block.type=='tool_call' and 'tool' or 'reasoning',
            toolName=block.toolName,callId=block.callId,arguments=block.arguments,text=block.text,
            title=block.type=='text' and 'Message' or nil}
          current.activities[#current.activities+1]=item
          current.count=#current.activities
          if block.callId then calls[block.callId]=item end
        end
      end
    end
  end
  local last=groups[#groups]
  if active and (not last or not last.compacting) then last=group() end
  if last then
    if running then last._started_at=started end
    if #groups==1 and last.label~='Compaction' then last.duration_ms=part.duration_ms; last.model_call=part.model_statistics end
  end
  return groups
end
function M.records(workflow,current,index,recap)
  local state=workflow.state
  local step,plan=state.steps[index+1],state.plan.steps[index+1]
  if state.status=='revising' and index>=(state.review_step or 0) then
    local empty={complete=false,explanation='',activity={}}
    local parts={}
    for n=1,#plan.substeps do parts[n]=empty end
    step=vim.tbl_extend('force',step,{whole=empty,substeps=parts,validation=empty,correction_request=state.proposed_rework})
  end
  local key=current.context_key..'/'..tostring(step.display_revision or 0)
  local records={}
  local function text(id,content,group,control)
    records[#records+1]={id=key..'/'..id,role='construction',content=content or '',group=group,control=control}
  end
  text('title',plan.title,'SosHeading')
  local connected=workflow.session.client.transport.state=='ready'
  text('status',connected and workflow:step_label(index) or 'Disconnected · :SosReconnect to refresh implementation','SosActivity')
  if not recap then
    local summary=step.correction_request
    if not summary or summary=='' then summary=step.whole.explanation end
    if not summary or summary=='' then summary=plan.objective end
    text('objective',summary)
    local done,total=M.progress(step)
    records[#records+1]={id=key..'/progress',role='construction',progress={done=done,total=total,
      validating=connected and not workflow.archive_view and not workflow.info.implementation_archive
        and index==state.work_step and state.status=='running' and (step.validation or {}).started and not step.whole.complete}}
  end
  local running=workflow.session.client.transport.state=='ready' and not workflow.archive_view and not workflow.info.implementation_archive and state.status=='running'
    and index==state.work_step and not state.preparing and not step.preparing
  local working=not workflow.archive_view and not workflow.info.implementation_archive
    and (running or state.status=='interrupted' or state.status=='failed')
    and index==state.work_step and not state.preparing and not step.preparing
  local function append(part,id,active)
    for _,record in ipairs(activity(part,key..'/'..id,running and active,workflow.active_started,active)) do records[#records+1]=record end
  end
  for n,definition in ipairs(plan.substeps) do
    local part=step.substeps[n] or {}
    text('part/'..n..'/title','\n'..n..'. '..(type(definition)=='table' and definition.title or definition),
      part.unnecessary and 'SosUnnecessary' or 'SosHeading')
    if not recap then append(part,'part/'..n,working and state.work_substep==n-1 and not (step.validation or {}).started) end
    if part.complete or part.announced or part.unnecessary or recap then
      text('part/'..n..'/description',part.unnecessary and 'No longer necessary given the changes in previous steps.' or part.explanation)
    end
  end
  if not recap then
    text('validation/title','\nValidating the step','SosActivity')
    append(step.validation or {},'validation',working and (step.validation or {}).started and not (step.validation or {}).complete)
  end
  local eligible=workflow:eligible(index,#step.substeps) and not (state.proposed_rework and state.proposed_rework~='')
  local review=workflow.session:review_actions()
  if review.importing and review.importing.identity==step.identity then text('pending','\nApplying changes · Awaiting daemon confirmation','SosActivity')
  elseif review.pending then text('pending','\nReview action pending · Awaiting daemon confirmation','SosActivity')
  elseif recap and eligible then text('validate','\n[ Validate and import step ]','SosAction','validate')
  elseif eligible then text('human','\n[ Human validation ]','SosAction','human_validation') end
  if eligible and not review.pending then text('review','[ Review this step ]','SosAction','review_question') end
  if not workflow.archive_view and not workflow.info.implementation_archive and index==state.work_step then
    if state.status=='running' then text('cancel','\n[ Cancel construction ]','SosAction','cancel')
    elseif state.status=='interrupted' or state.status=='failed' then text('continue','\n[ Continue ]','SosAction','continue') end
  end
  if state.error and state.error~='' and index==state.work_step then text('error',state.error,'SosError') end
  return records
end
return M
