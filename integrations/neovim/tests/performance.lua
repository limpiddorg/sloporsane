-- Keep timer work independent of history size and batch network-driven redraws.
local api=vim.api
local Render=require('sos.render')
local Highlights=require('sos.highlights')
local Session=require('sos.session')
local Panel=require('sos.panel')
local calls={}
local originals={project=Render.project,apply=Highlights.apply,capture=Panel.capture_folds}
Render.project=function(...) calls.project=(calls.project or 0)+1; return originals.project(...) end
Highlights.apply=function(...) calls.apply=(calls.apply or 0)+1; return originals.apply(...) end
Panel.capture_folds=function(...) calls.capture=(calls.capture or 0)+1; return originals.capture(...) end
local client={transport={state='ready',pending={}},conversation={},operations={}}
function client:request() error('Performance fixture must not load historical bodies') end
local session=Session.new(client,{workspace='/project',progress=false})
local panel=Panel.new(session)
panel:show()
local measurements={}
for _,count in ipairs({10,50,100}) do
  local records,last={}
  for n=1,count do
    records[#records+1]={id='u'..n,role='user',content='Inspect this code'}
    local tools={}
    for index=0,4 do
      tools[#tools+1]={live=true,activityIndex=index,kind='tool',toolName='bash',
        arguments={mainCommand='pytest',command='pytest -q'},result={output='2 passed',isError=false}}
    end
    last={id='act'..n,role='activity',count=5,activities=tools}
    records[#records+1]=last
    records[#records+1]={id='a'..n,role='assistant',content=string.rep('Some **generated** explanation with `code`.\n',10)}
  end
  session.current.records=records
  panel:render()
  local projection=session.current.view.projection
  local buffer=session.current.view.transcript
  assert(Render.get(buffer)==projection,'Native folds must share the Lua projection without copying it through vim.b')
  api.nvim_win_call(panel.transcript,function()
    vim.b.sos_foldlevels={99}
    for line,level in ipairs(projection.levels) do
      assert(Render.fold(line)==level,'Fold expression read a buffer-variable copy instead of the Lua projection')
    end
    vim.b.sos_foldlevels=nil
  end)
  calls={}
  local started=vim.uv.hrtime()
  for _=1,5 do panel:render() end
  local render_ms=(vim.uv.hrtime()-started)/1e6/5
  assert(not calls.capture and not calls.apply,'Unchanged rendering rebuilt folds or highlights')
  last._started_at=vim.uv.hrtime()/1e6-1000
  panel:render()
  projection=session.current.view.projection
  local row=projection.anchors['activity:'..last.id]
  local span=projection.live_activities[1].span
  api.nvim_win_call(panel.transcript,function()
    api.nvim_win_set_cursor(panel.transcript,{span.start,0})
    vim.cmd('normal! zo')
  end)
  panel:sync_folds()
  local mark=api.nvim_buf_set_extmark(buffer,Highlights.namespace,0,0,{virt_text={{'retained','SosMuted'}}})
  local writes={}
  api.nvim_buf_attach(buffer,false,{on_lines=function(_,_,_,first,old_end,new_end)
    writes[#writes+1]={first,old_end,new_end}
  end})
  calls={}
  local levels=projection.levels
  local before=api.nvim_win_call(panel.transcript,vim.fn.winsaveview)
  started=vim.uv.hrtime()
  for frame=1,10 do
    last._started_at=vim.uv.hrtime()/1e6-1000-frame*200
    panel:tick()
  end
  local tick_ms=(vim.uv.hrtime()-started)/1e6/10
  assert(not calls.project and not calls.apply and not calls.capture,
    'Timer projected history, rebuilt highlights or captured every fold')
  assert(projection==session.current.view.projection and projection.levels==levels,'Timer replaced the fold structure')
  assert(#writes==10,'Timer did not update the Activity header')
  for _,write in ipairs(writes) do
    assert(vim.deep_equal(write,{row-1,row,row}),'Timer changed more than its Activity row: '..vim.inspect(write))
  end
  assert(#api.nvim_buf_get_extmark_by_id(buffer,Highlights.namespace,mark,{})>0,'Timer cleared unrelated highlights')
  assert(api.nvim_win_call(panel.transcript,function() return vim.fn.foldclosed(span.start) end)==-1,'Timer closed an open Activity')
  local after=api.nvim_win_call(panel.transcript,vim.fn.winsaveview)
  assert(before.lnum==after.lnum and before.col==after.col and before.topline==after.topline,'Timer moved the reader')
  measurements[#measurements+1]={turns=count,lines=#projection.lines,render_ms=render_ms,tick_ms=tick_ms}
  last._started_at=nil
end
-- A resize racing the next timer must reflow first, not reuse stale mark columns.
api.nvim_win_set_width(panel.transcript,20)
panel:tick()
assert(panel.render_pending,'Timer reused a projection from before the resize')
assert(vim.wait(1000,function() return not panel.render_pending end,10),'Resize did not complete its reflow')
assert(session.current.view.width==20,'Resize did not replace the cached projection width')
panel:hide()
local buffer=session.current.view.transcript
api.nvim_buf_delete(buffer,{force=true})
assert(not Render.get(buffer),'Deleted transcript retained its projection cache')

-- Real transport dispatch includes terminal callbacks, without delaying state updates.
local live_client=require('sos.client').new({})
live_client.transport.state='ready'
local live=Session.new(live_client,{workspace='/project',progress=false})
local live_panel=Panel.new(live)
live_panel:show()
calls={}
for _=1,20 do live_client.transport:receive({type='mode.updated',mode='slop'}) end
assert(live.current.mode=='slop','Batching delayed daemon acknowledgement state')
assert(not calls.project and live_panel.render_pending,'Network frames rendered synchronously')
assert(vim.wait(1000,function() return not live_panel.render_pending end,10),'Batched render never completed')
assert(calls.project==1,'Network burst did not coalesce into one projection')
assert(vim.wo[live_panel.input].winbar:find('SosModeSlop',1,true),'Batched render lost the latest mode')
calls={}
live_client.transport.pending.reply={terminal='mode.updated',callback=function() live:changed() end}
live_client.transport:receive({type='mode.updated',mode='sane',requestId='reply'})
assert(vim.wait(1000,function() return not live_panel.render_pending end,10))
assert(calls.project==1,'Terminal callback and its event caused duplicate projections')
live_client.transport:receive({type='mode.updated',mode='slop'})
live_panel:hide()
assert(not live_panel.render_pending,'Hidden panel retained queued rendering')
Render.project=originals.project
Highlights.apply=originals.apply
Panel.capture_folds=originals.capture
vim.fn.mkdir('build-ai/neovim','p')
local version=vim.version()
vim.fn.writefile({vim.json.encode(measurements)},'build-ai/neovim/performance-'..version.major..'.'..version.minor..'.'..version.patch..'.json')
for _,row in ipairs(measurements) do
  print(string.format('%d lines: full render %.2f ms, Activity-only tick %.3f ms',row.lines,row.render_ms,row.tick_ms))
end
print('Direct fold cache, targeted Activity ticks, preserved native folds and batched network rendering passed')
