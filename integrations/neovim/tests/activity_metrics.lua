-- Activity visibility and metrics must match live and preserved TUI evidence.
local History=require('sos.history')
local Tools=require('sos.tool_format')
local Render=require('sos.render')
local Session=require('sos.session')
local Panel=require('sos.panel')
local function header(record) return Tools.activity(record,{}) end
assert(header({count=0,metadata={duration_ms=5000},model_call={duration_ms=2000,output_tokens=100}})=='Activity 5s - 50 tok/s')
assert(header({count=2,duration_ms=12000,model_call={duration_ms=4000,output_tokens=103}})=='Activity 2 steps | 12s - 25.8 tok/s')
assert(header({count=0,metadata={duration_ms=5000},model_call={duration_ms=2000,output_tokens=0}})=='Activity 5s - 0 tok/s')
for _,call in ipairs({{duration_ms=2000},{duration_ms=0,output_tokens=100},{duration_ms=2000,output_tokens=-1},
  {duration_ms=2000,output_tokens=math.huge},{duration_ms=math.huge,output_tokens=100}}) do
  assert(not header({metadata={duration_ms=5000},model_call=call}):find('tok/s',1,true),'Unknown or invalid usage fabricated a rate')
end
assert(not header({model_call={duration_ms=2000,output_tokens=100}}):find('tok/s',1,true),'Legacy timing must stay unknown')

local clock=100
local current={records={}}
local history=History.new({},current)
history.clock=function() return clock end
history:start_turn()
clock=500
history:live({messages={{id='u',role='user',content={{type='text',text='Question'}}}}})
local first=history.active
assert(first.count==0 and first._started_at==100)
local empty=Render.project(current,{})
assert(#empty.spans==1 and empty.spans[1].start==empty.spans[1].finish and not empty.spans[1].group)
local function response(id,call,blocks,metadata,elapsed)
  local message={id=id,role='assistant',content=blocks,model_call=call,metadata=metadata,elapsed_ms=elapsed}
  history:live({messages={message}})
  return message
end
clock=2500
local tool=response('m1',{duration_ms=2000,output_tokens=100},{{type='tool_call',callId='c',toolName='glob',arguments={pattern='*.lua'}}},nil,2400)
history:live({messages={tool}})
assert(first.count==1 and first.model_call.output_tokens==100,'Duplicate source counted twice')
clock=4500
history:live({messages={{id='result',role='tool',content={{type='tool_result',callId='c',output='a.lua',isError=false}}}}})
assert(first.model_call.duration_ms==2000,'Tool execution was counted as model time')
clock=6500
response('m2',{duration_ms=1000,output_tokens=50},{{type='text',text='Interim'},{type='tool_call',callId='c2',toolName='read',arguments={filePath='a.lua'}}},nil,6400)
local second=history.active
assert(second~=first and first.terminal==false and first.duration_ms==6400)
assert(first.model_call.duration_ms==3000 and first.model_call.output_tokens==150)
assert(header(first):find('50 tok/s',1,true),'Throughput must be total output divided by model time, not wall time')
clock=9500
response('m3',{duration_ms=3000,output_tokens=90},{{type='text',text='Answer'}},{model='model',reasoning='high',duration_ms=9400},9400)
history:live({type='execution.completed',metadata={model='model',reasoning='high',duration_ms=9400}})
assert(not second._started_at and second.metadata.duration_ms==9400 and second.model_call.output_tokens==90)
assert(header(second):find('9s - 30 tok/s',1,true))
clock=30000
history:finish()
assert(second.metadata.duration_ms==9400,'Revisiting or finishing twice changed preserved timing')

-- A plain answer still completes the initially empty Activity, with no lazy details.
history:live({messages={{id='u2',role='user',content={{type='text',text='Plain question'}}}}})
response('plain',{duration_ms=1000,output_tokens=25},{{type='text',text='Plain answer'}},{duration_ms=1500},1500)
assert(history.active.count==0 and header(history.active)=='Activity 1s - 25 tok/s')

-- Missing usage in any counted model response poisons the whole aggregate.
history:live({messages={{id='u3',role='user',content={{type='text',text='Unknown usage'}}}}})
response('unknown',{duration_ms=1000},{{type='tool_call',callId='c3',toolName='glob',arguments={pattern='*'}}})
response('known',{duration_ms=1000,output_tokens=10},{{type='text',text='Done'}},{duration_ms=3000})
assert(history.active.model_call.output_tokens==nil and not header(history.active):find('tok/s',1,true))
history:live({messages={{id='u4',role='user',content={{type='text',text='Overflow'}}}}})
response('huge',{duration_ms=9007199254740991,output_tokens=1},{{type='tool_call',callId='c4',toolName='glob',arguments={pattern='*'}}})
response('overflow',{duration_ms=1,output_tokens=1},{{type='text',text='Done'}},{duration_ms=3000})
assert(not header(history.active):find('tok/s',1,true))

-- Failure/cancellation have timing even when no final message arrives.
for _,terminal in ipairs({'execution.failed','execution.cancelled'}) do
  history:live({messages={{id=terminal,role='user',content={{type='text',text='Question'}}}}})
  local active=history.active
  clock=clock+1000
  history:live({type=terminal,metadata={duration_ms=2000}})
  assert(not active._started_at and header(active)=='Activity 2s')
end
history:live({messages={{id='disconnect',role='user',content={{type='text',text='Question'}}}}})
clock=clock+1250
history:finish()
assert(history.active.duration_ms==1250 and not history.active._started_at)

-- Pure construction/history projection must never start a local live timer.
local preserved=History.project({{id='pu',role='user',content={{type='text',text='Question'}}},
  {id='pa',role='assistant',content={{type='text',text='Answer'}},model_call={duration_ms=1000,output_tokens=40},metadata={duration_ms=2000}}},'preserved')
assert(not preserved[2]._started_at and header(preserved[2])=='Activity 2s - 40 tok/s')

-- Time refreshes while idle without moving reading position, stealing focus or loading details.
local client={transport={state='ready',pending={}},conversation={},operations={}}
function client:request() error('Empty Activity must not request historical payloads') end
local session=Session.new(client,{workspace='/project',progress=false})
local live=session:history()
live:live({messages={{id='live-user',role='user',content={{type='text',text=string.rep('Wait\n',40)}}}}})
live.active._started_at=vim.uv.hrtime()/1e6-990
local panel=Panel.new(session)
panel:show()
local focus=vim.api.nvim_get_current_win()
local position=vim.api.nvim_win_get_cursor(panel.transcript)
local before=table.concat(session.current.view.projection.lines,'\n')
assert(vim.wait(1500,function() return table.concat(session.current.view.projection.lines,'\n')~=before end,20),
  'Activity time did not refresh between daemon events')
assert(vim.api.nvim_get_current_win()==focus and vim.deep_equal(vim.api.nvim_win_get_cursor(panel.transcript),position))
vim.api.nvim_win_call(panel.transcript,function() vim.cmd('normal! zR') end)
panel:sync_folds()
live:live({type='execution.completed',metadata={duration_ms=2000}})
session:changed()
assert(table.concat(session.current.view.projection.lines,'\n'):find('Activity 2s',1,true))
panel:hide(); panel:show()
assert(table.concat(session.current.view.projection.lines,'\n'):find('Activity 2s',1,true))
panel:hide()
print('Visible empty Activities, live timers, preserved throughput and deduplicated model metrics passed')
