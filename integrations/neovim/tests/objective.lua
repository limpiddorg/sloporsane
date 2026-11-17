-- Objective drawers are read-only; implementation is an explicit latest-message action.
local api=vim.api
local Session=require('sos.session')
local Render=require('sos.render')
local Highlights=require('sos.highlights')
local sos=require('sos')
local previous_select,previous_session,previous_panel=require('sos.ui').select,sos.session,sos.panel
local client={transport={state='ready',pending={}},conversation={},operations={}}
function client:request() error('Objective selection sent an unexpected daemon request') end
local s=Session.new(client,{workspace='/project',progress=false})
s.configured=true
s.current.id='discussion'
local current=s.current
local w=s.workflow
local panel=require('sos.panel').new(s)
local objective={id='bound-plan',content='# Build the feature\n**Keep** the draft and `code`.',objective_stack='Lua'}
local newer={id='newer-plan',content='Do not select this unbound version'}
local popups,opened={},{}
local original_open=w.open_objective
w.open_objective=function(_,value,auto,pace) opened[#opened+1]={value=value,auto=auto,pace=pace} end
local function reset(records)
  s.current=current
  current.kind,current.preview,current.archived='discussion',nil,nil
  current.busy,current.pending,s.switching=false,nil,nil
  current.draft='Retained draft'
  s:history():invalidate()
  s:history():install(records or {{id='answer',role='assistant',content='Answer'},
    {id='after-answer',role='activity',count=0}}, {})
  s.info={objective_plans={objective,newer},objective_plan_turns={{message_id='answer',objective_id=objective.id}}}
  w.info=s.info
  s:changed()
end
local function menu()
  local count=#popups
  vim.cmd('SosImplement')
  assert(#popups==count+1,'SosImplement did not offer the implementation choices')
  return popups[#popups]
end
local function refused(fragment)
  local count,actions=#popups,#opened
  vim.cmd('SosImplement')
  assert(#popups==count and #opened==actions,'Refused implementation still opened a popup or started work')
  assert(s.current.notice:find(fragment,1,true),s.current.notice)
end
local ok,err=xpcall(function()
  require('sos.ui').select=function(rows,options,callback)
    popups[#popups+1]={rows=rows,options=options,callback=callback}
  end
  sos.session=function() return s end
  sos.panel=function() return panel end
  vim.cmd('runtime plugin/sos.lua')
  assert(api.nvim_get_commands({}).SosImplement,'Public SosImplement command is missing')
  assert(vim.fn.maparg('<Plug>(SosImplement)','n')~='','Public SosImplement binding is missing')
  reset()
  local selection=menu()
  assert(vim.deep_equal(selection.rows,{'Implementation plan','Implement (Auto plan)'}))
  assert(#opened==0 and current.draft=='Retained draft','Opening the popup changed the conversation')
  selection.callback(nil)
  assert(#opened==0,'Canceling the implementation popup started work')
  selection=menu(); selection.callback(selection.rows[1])
  assert(#opened==1 and opened[1].value==objective and opened[1].auto==false,
    'Manual planning did not use the plan attached to the last message')
  -- Exercise the public <Plug> action, not just the command.
  local count=#popups
  vim.fn.maparg('<Plug>(SosImplement)','n',false,true).callback()
  assert(#popups==count+1)
  selection=popups[#popups]; selection.callback(selection.rows[2])
  local pace=popups[#popups]
  assert(pace.rows[1].value=='step_by_step' and pace.rows[2].value=='everything')
  pace.callback(nil)
  assert(#opened==1,'Canceling the pace popup started work')
  for _,index in ipairs({1,2}) do
    selection=menu(); selection.callback(selection.rows[2])
    pace=popups[#popups]; pace.callback(pace.rows[index])
    assert(opened[#opened].value==objective and opened[#opened].auto==true
      and opened[#opened].pace==pace.rows[index].value,'Automatic planning lost the objective or selected pace')
  end
  assert(current.draft=='Retained draft' and not current.pending and not current.busy)
  local projection=current.view.projection
  assert(not projection.actions and next(projection.plan_lines))
  for line in pairs(projection.plan_lines) do assert(not projection.bands[line],'Objective stripe remains') end
  assert(projection.bands[projection.anchors['message:answer:1']]==nil)
  local span
  for _,value in ipairs(projection.spans) do if value.key=='turn-plan:answer' then span=value end end
  assert(span and span.highlight=='SosObjectivePlan' and projection.foldtext[span.start][1][2]=='SosObjectivePlan',
    'Folded objective title lost its semantic highlight')
  local body=projection.anchors['turn-plan:answer/content/1']
  assert(projection.highlights_by_line[body][1].group=='SosPlanBody')
  assert(api.nvim_get_hl(0,{name='SosPlanBody',link=true}).link=='Normal',
    'Objective body must inherit normal text, not the green updated-title accent')
  assert(api.nvim_get_hl(0,{name='SosObjectivePlan'}).fg==0x71808E)
  local updated=Render.project({records={{id='answer',role='assistant',content='Answer'}}},
    {objective_plans={objective},objective_plan_turns={{message_id='answer',objective_id=objective.id,updated=true}}})
  local title=updated.anchors['turn-plan:answer']
  assert(updated.foldtext[title][1][2]=='SosObjectivePlanUpdated')
  assert(updated.highlights_by_line[updated.anchors['turn-plan:answer/separator']][1].group=='SosSane')
  assert(updated.highlights_by_line[updated.anchors['turn-plan:answer/stack-heading']][1].group=='SosHeading')
  for _,mark in ipairs(updated.highlights) do
    if mark.group=='SosObjectivePlanUpdated' or mark.group=='SosSane' then
      assert(mark.line<=title,'Green Objective Plan accent leaked into the Markdown body or stack')
    end
  end
  local content=updated.anchors['turn-plan:answer/content/2']
  local groups={}
  for _,mark in ipairs(updated.highlights_by_line[content]) do groups[mark.group]=true end
  assert(groups.SosBold and groups.SosCode,'Objective Markdown lost its inline formatting')
  for _,line in ipairs(projection.lines) do
    assert(not line:find('[ Implementation',1,true) and not line:find('[ Implement (Auto plan)',1,true))
  end
  local actions=#opened
  api.nvim_win_call(panel.transcript,function()
    api.nvim_win_set_cursor(panel.transcript,{span.start,0})
    local enter=vim.fn.maparg('<CR>','n',false,true).callback
    enter(); assert(vim.fn.foldclosed(span.start)==-1,'Enter no longer opens the objective fold')
    enter(); assert(vim.fn.foldclosed(span.start)==span.start,'Enter no longer closes the objective fold')
  end)
  assert(#opened==actions,'Reading an objective launched implementation')
  reset({{id='answer',role='assistant',content=''}})
  selection=menu(); selection.callback(nil)
  reset({})
  refused('An Objective Plan is required in the last assistant message')
  reset({{id='user',role='user',content='Please implement'}})
  refused('An Objective Plan is required')
  reset({{id='answer',role='assistant',content='Old answer'},
    {id='later',role='assistant',content='Objective plan\nThis is only ordinary Markdown'}})
  refused('An Objective Plan is required')
  reset({{id='answer',role='assistant',content='Old answer'},{id='later-user',role='user',content='New question'}})
  refused('An Objective Plan is required')
  reset(); w.info.objective_plan_turns[1].objective_id='missing-plan'
  refused('An Objective Plan is required')
  reset(); current.busy=true; refused('Wait for the current turn')
  reset(); current.pending={text='Pending'}; refused('Wait for the current turn')
  reset(); s.switching='another'; refused('Wait for the current turn')
  reset(); current.preview=true; refused('Return to an idle Discussion')
  reset(); current.archived=true; refused('Return to an idle Discussion')
  reset(); current.kind='planning'; refused('Return to an idle Discussion')
  reset(); s:history().loading=true; refused('Return to the latest Discussion message')
  reset(); s:history().page.hasNewer=true; refused('Return to the latest Discussion message')
  reset(); selection=menu(); actions=#opened
  current.records[#current.records+1]={id='later',role='assistant',content='Later answer'}
  s:changed(); selection.callback(selection.rows[1])
  assert(#opened==actions and current.notice:find('Run :SosImplement again',1,true))
  reset(); selection=menu(); selection.callback(selection.rows[2]); pace=popups[#popups]
  w.info.objective_plan_turns[1].objective_id=newer.id
  pace.callback(pace.rows[1])
  assert(#opened==actions and current.notice:find('Run :SosImplement again',1,true),
    'Changing the bound objective did not invalidate the pace popup')
  reset(); selection=menu()
  s.current={id='other',kind='discussion',mode='sane',records={},draft='',workspace='/project'}
  s:changed(); selection.callback(selection.rows[1])
  assert(#opened==actions and s.current.notice:find('Run :SosImplement again',1,true))
  reset(); selection=menu(); current.pending={text='New pending turn'}
  selection.callback(selection.rows[1])
  assert(#opened==actions,'A pending send did not invalidate the open popup')
  -- Existing branches reopen, even through the automatic choice, without retrying generation.
  reset()
  local frames={}
  w.view=function(_,frame,after) frames[#frames+1]=frame; after() end
  w.command=function(_,name,fields) frames[#frames+1]={type=name,pace=fields.pace} end
  w.generate=function() frames[#frames+1]={type='generate'} end
  w.info.implementation_branches={{id='existing',objective_id=objective.id}}
  original_open(w,objective,true,'everything')
  assert(#frames==1 and frames[1].type=='implementation.view' and frames[1].branchId=='existing')
  w.info.implementation_branches={}
  original_open(w,objective,true,'step_by_step')
  assert(frames[2].type=='implementation.open' and frames[2].objectiveId==objective.id)
  assert(frames[3].type=='implementation.auto_plan' and frames[3].pace=='step_by_step')
  original_open(w,objective,false)
  assert(frames[4].type=='implementation.open' and frames[5].type=='generate')
  -- The same public action validates a finalized manual implementation plan.
  local function planning()
    reset()
    current.kind='planning'
    w.branch_id='branch'
    w.info.implementation_plan_ready=true
    w.info.implementation_plan={content='Final implementation plan'}
    w.info.implementation_execution=false
    w.state,w.archive_view=nil,nil
  end
  for _,index in ipairs({1,2}) do
    planning()
    local count=#frames
    selection=menu()
    assert(selection.options.prompt=='Implementation pace' and selection.rows[index].value,
      'Finalized implementation plan reopened the objective-mode selector')
    assert(#frames==count,'Opening the pace selector accepted the plan')
    selection.callback(nil)
    assert(#frames==count,'Canceling manual plan validation started construction')
    selection=menu(); selection.callback(selection.rows[index])
    assert(frames[#frames].type=='implementation.start' and frames[#frames].pace==selection.rows[index].value)
    assert(not w.state and current.kind=='planning','Starting construction optimistically changed daemon-owned state')
  end
  planning(); current.kind='plan'
  selection=menu(); selection.callback(selection.rows[1])
  assert(frames[#frames].type=='implementation.start','SosImplement cannot validate the displayed implementation plan')
  planning(); current.busy=true; refused('Wait for implementation planning to finish')
  planning(); w.info.implementation_plan_ready=false; refused('A finalized implementation plan is required')
  planning(); w.info.implementation_archive=true; refused('Implementation is not awaiting plan validation')
  planning(); w.state={status='running'}; refused('Implementation is not awaiting plan validation')
  planning(); selection=menu()
  local count=#frames
  w.info.implementation_plan={content='Changed plan'}
  selection.callback(selection.rows[1])
  assert(#frames==count and current.notice:find('Implementation planning changed',1,true))
  planning(); selection=menu(); w.branch_id='different-branch'
  selection.callback(selection.rows[1])
  assert(#frames==count,'Changing branches did not invalidate manual plan validation')
end,debug.traceback)
panel:hide()
require('sos.ui').select,sos.session,sos.panel=previous_select,previous_session,previous_panel
assert(ok,err)
print('TUI-aligned objective colors, latest-message SosImplement choices and stale-popup safeguards passed')
