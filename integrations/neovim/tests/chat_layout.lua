-- Left edges and tool bodies follow app.py, turn_activity.py and tool_view.py.
local api=vim.api
if #api.nvim_list_uis()==0 then
  -- 0.10 does not allocate a headless screen. Attach a real RPC UI for cell checks.
  local child=vim.fn.jobstart({vim.v.progpath,'--embed','--headless','-u','NONE'},{rpc=true})
  assert(child>0,'Could not launch Neovim screen fixture')
  local ok,err=pcall(function()
    vim.rpcrequest(child,'nvim_ui_attach',80,24,{})
    vim.rpcrequest(child,'nvim_exec_lua',[[
      local root=...
      vim.opt.runtimepath:prepend(vim.env.SOS_TEST_RUNTIME or root..'/integrations/neovim')
      dofile(root..'/integrations/neovim/tests/chat_layout.lua')
    ]],{vim.fn.getcwd()})
  end)
  vim.fn.jobstop(child)
  assert(ok,err)
  print('TUI chat offsets, Activity types, tool bodies and native fold screen layout passed')
  return
end
local Render=require('sos.render')
local Tools=require('sos.tool_format')
local Session=require('sos.session')
local Panel=require('sos.panel')
local original={lines=vim.o.lines,columns=vim.o.columns,fillchars=vim.go.fillchars}
vim.o.lines=24
vim.o.columns=80
vim.go.fillchars='fold:·'
local glob={live=true,activityIndex=0,kind='tool',toolName='glob',arguments={path='src',pattern='**/*.lua'},
  result={output='src/main.lua\nsrc/init.lua',isError=false}}
local shell={live=true,activityIndex=1,kind='tool',toolName='bash',arguments={mainCommand='pytest',command='pytest -q',workdir='/project'},
  result={output='2 passed\n\n<shell_metadata>\nExit code: 0\n</shell_metadata>',isError=false,metadata={exit=0}}}
local reasoning={live=true,activityIndex=2,kind='reasoning',text='Check the existing API first.'}
local read={live=true,activityIndex=3,kind='tool',toolName='read',arguments={filePath='src/main.lua'},result={isError=false}}
local records={
  {id='user',role='user',content='Find the Lua sources.',metadata={mode='sane'}},
  {id='first',role='activity',count=4,kinds={'tools','reasoning','read'},duration_ms=62000,activities={glob,shell,reasoning,read}},
  {id='answer',role='assistant',content='The sources are in src.'},
  {id='second',role='activity',count=1,activities={glob}},
  {id='next',role='user',content='Thanks',metadata={mode='slop'}},
}
local projection=Render.project({mode='slop',records=records},{},60)
local function at(key) return projection.lines[assert(projection.anchors[key],key)] end
assert(at('message:user:1')=='   Find the Lua sources.')
assert(at('activity:first')=='   ▾ Activity ▪ ▪ ▪ 4 steps | 1m 02s')
assert(at('activity:first/item/0')=='     ▾ glob · Completed • **/*.lua')
assert(at('activity:first/item/1')=='     ▾ bash · Completed • pytest · Exit code 0')
assert(at('activity:first/item/2')=='     ▾ Reasoning')
assert(at('activity:first/item/3')=='       read · src/main.lua')
assert(at('activity:first/item/0/detail/1')=='         src')
assert(at('activity:first/item/0/detail/2')=='         **/*.lua')
assert(at('activity:first/item/0/detail/3')=='         '..string.rep('─',50))
assert(at('activity:first/item/0/detail/4')=='         src/main.lua')
assert(at('message:answer:1')==' The sources are in src.')
assert(at('activity:second/separator')==string.rep('─',60))
assert(at('message:next:1/spacing')=='')
for _,row in ipairs(Tools.details(glob)) do assert(row.text~='Path' and row.text~='Pattern' and row.text~='Result') end
local shell_rows=Tools.details(shell)
assert(shell_rows[1].text=='command: pytest -q' and shell_rows[2].text=='timeout: 120000 ms')
assert(shell_rows[3].text=='workdir: /project' and shell_rows[4].separator)
assert(shell_rows[5].text=='Exit code: 0' and shell_rows[6].text=='2 passed')
local grep=Tools.details({toolName='grep',arguments={pattern='foo'},result={output='Found 2 matches\n\nsrc/a.lua:\n  Line 3: foo',metadata={matches=2}}})
assert(grep[4].text=='2 Matches' and grep[6].marks[1].group=='SosRead')
local lazy=Render.project({records={{id='lazy',role='activity',count=2,kinds={'tools','read'}}}},{})
assert(lazy.lines[1]=='   ▾ Activity ▪ ▪ 2 steps','Type indicators must not require historical tool bodies')
local title,_,marks=Tools.title({toolName='bash',arguments={mainCommand='pytest'},result={isError=true,metadata={exit=1,permissions={{decision='ai_accepted',confidence=.9}}}}})
assert(title=='bash • AI 90% • Completed • pytest · Exit code 1')
assert(#marks==4)
local footer=Render.project({records={{id='a',role='assistant',content='Done',metadata={model='local',reasoning='none',duration_ms=2000}}}},{},40)
assert(footer.lines[2]==string.rep(' ',14)..'local | No reasoning | 2s','Generation footer must be right-aligned with a one-cell margin')

local client={transport={state='ready',pending={}},conversation={},operations={}}
function client:request() error('Chat layout must not load unrelated history') end
local session=Session.new(client,{workspace='/project',progress=false})
session:history():install(records,{})
local panel=Panel.new(session)
panel:show()
assert(api.nvim_win_get_height(panel.input)==5,'Initial composer height must be five lines')
local function screen_row(row,width,start)
  local cells={}
  for col=start or 1,width do cells[#cells+1]=vim.fn.screenstring(row,col) end
  return table.concat(cells)
end
local snapshots={}
for _,width in ipairs({60,40}) do
  api.nvim_win_set_width(panel.transcript,width)
  panel:render()
  api.nvim_set_current_win(panel.transcript)
  api.nvim_win_set_cursor(panel.transcript,{1,0})
  vim.cmd('normal! zMzt')
  vim.cmd('redraw!')
  local pos=api.nvim_win_get_position(panel.transcript)
  local row,col=pos[1]+2,pos[2]+1 -- winbar, then content; no fold/sign columns
  local top=screen_row(row-1,vim.o.columns,col)
  assert(top==' Discussion'..string.rep(' ',width-15)..'dev ','Transcript header must contain only context and right-aligned version: '..top)
  local tab_attr=vim.fn.screenattr(row-1,col)
  for column=col,col+11 do
    assert(vim.fn.screenattr(row-1,column)==tab_attr,'Blue tab background must include the padded context label')
  end
  assert(vim.fn.screenattr(row-1,col+12)~=tab_attr,'Active tab background leaked into the rest of the header')
  assert(vim.fn.screenattr(row-1,col+width-4)~=tab_attr,'Version inherited the active tab style')
  assert(vim.fn.screenstring(row,col+1)=='█','User band is not a full cell at offset 1, width '..width..' at '..row..','..col..' columns '..vim.o.columns..': '..screen_row(row,vim.o.columns,col))
  assert(vim.fn.screenstring(row,col+3)=='F','User text is not at offset 3')
  local folded=screen_row(row+1,vim.o.columns,col)
  assert(folded:match('^   ▸ Activity'),'Collapsed Activity moved away from offset 3: '..folded)
  assert(not folded:find('·',1,true) and folded:sub(-3)=='   ','Collapsed Activity must have blank filler: '..folded)
  local foldline=session.current.view.projection.anchors['activity:first']
  local chunks=Render.get(session.current.view.transcript).foldtext[foldline]
  assert(chunks[1][2]=='SosActivity','Collapsed Activity lost its grey semantic highlight')
  vim.cmd('normal! zRzt')
  vim.cmd('redraw!')
  local toolrow=screen_row(row+2,vim.o.columns,col)
  assert(toolrow:match('^     ▾ glob'),'Expanded tool is not at offset 5: '..toolrow)
  local detailrow=screen_row(row+3,vim.o.columns,col)
  assert(detailrow:match('^         src'),'Tool arguments are not at offset 9: '..detailrow)
  local shell_line=session.current.view.projection.anchors['activity:first/item/1']
  assert(vim.fn.strdisplaywidth(session.current.view.projection.lines[shell_line])<width,'Tool title wrapped instead of fitting its one-row header')
  snapshots[#snapshots+1]='Width '..width
  for y=row,row+12 do snapshots[#snapshots+1]=screen_row(y,vim.o.columns,col) end
end
session.info={model='gpt',reasoning='high',last_turn_used_context=1250,total_context=128000}
api.nvim_win_set_width(panel.input,60)
session:changed()
api.nvim_set_current_win(panel.input)
vim.cmd('redraw!')
local input_pos=api.nvim_win_get_position(panel.input)
local info_row=screen_row(input_pos[1]+1,vim.o.columns,input_pos[2]+1)
assert(info_row:match('^ gpt | high') and info_row:sub(-15)=='     Sane      ',info_row)
local badge_attr=vim.fn.screenattr(input_pos[1]+1,input_pos[2]+60)
for column=46,60 do
  assert(vim.fn.screenattr(input_pos[1]+1,input_pos[2]+column)==badge_attr,
    'Mode background must cover exactly the rightmost 15 cells')
end
assert(vim.fn.screenattr(input_pos[1]+1,input_pos[2]+45)~=badge_attr,'Mode background extends beyond the TUI button width')
local context='1 250 / 128 000 [1%]'
assert(info_row:find(context,1,true)==math.floor((60-#context)/2)+1,'Composer context is not visually centered: '..info_row)
assert(vim.wo[panel.input].winbar:find('SosModeSane',1,true))
local header_pos=api.nvim_win_get_position(panel.transcript)
assert(vim.fn.screenattr(header_pos[1]+1,header_pos[2]+2)~=vim.fn.screenattr(header_pos[1]+1,header_pos[2]+60-3),
  'Nonfocused transcript lost its distinct tab/version styles')
-- Native wrapping must retain both the offset and the full-cell band.
session.current.records={{id='wrapped',role='user',content=string.rep('word ',30),metadata={mode='sane'}}}
session:changed()
api.nvim_win_set_cursor(panel.transcript,{1,0})
vim.cmd('normal! zt')
vim.cmd('redraw!')
local pos=api.nvim_win_get_position(panel.transcript)
assert(vim.fn.screenstring(pos[1]+3,pos[2]+2)=='█','Wrapped user line lost its band')
assert(vim.fn.screenstring(pos[1]+3,pos[2]+4)~=' ','Wrapped user text lost its left offset')
session.current.records={{id='plain-user',role='user',content='No tools'},
  {id='empty',role='activity',count=0,metadata={duration_ms=12000},model_call={duration_ms=2000,output_tokens=50}}}
session:changed()
api.nvim_win_set_cursor(panel.transcript,{1,0})
vim.cmd('normal! zMzt')
vim.cmd('redraw!')
assert(screen_row(pos[1]+3,vim.o.columns,pos[2]+1):match('^   ▸ Activity 12s %- 25 tok/s'),
  'Empty folded Activity timing and throughput are not visible on screen')
local discussion=session.current
session.workflow:select({kind='planning',mode='sane',draft='',records=discussion.records,workspace='/project'})
api.nvim_win_set_cursor(panel.transcript,{1,0})
vim.cmd('normal! zMzt')
vim.cmd('redraw!')
local switched=screen_row(pos[1]+3,vim.o.columns,pos[2]+1)
assert(switched:match('^   ▸ Activity') and not switched:find('·',1,true) and switched:sub(-3)=='   ',
  'Switching context restored dotted fold filler: '..switched)
local top=screen_row(pos[1]+1,vim.o.columns,pos[2]+1)
assert(top==' Discussion  Implement'..string.rep(' ',60-26)..'dev ','Implement did not retain the Discussion tab: '..top)
assert(vim.fn.screenattr(pos[1]+1,pos[2]+2)~=vim.fn.screenattr(pos[1]+1,pos[2]+14),
  'Preserved Discussion must look inactive beside Implement')
session.workflow:select(discussion)
assert(vim.wo[panel.transcript].fillchars:find('fold: ',1,true),'Returning to Discussion restored dotted fold filler')
assert(vim.go.fillchars=='fold:·','Chat changed the editor fold filler globally')
-- A visible tail is not permission to move a reader's cursor on every refresh.
local running={id='live-activity',role='activity',count=0,_started_at=vim.uv.hrtime()/1e6-990}
local answer={id='live-answer',role='assistant',content='First line\nSecond line\nThird line'}
session.current.records={{id='live-question',role='user',content='Question'},running,answer}
session:changed()
api.nvim_set_current_win(panel.transcript)
vim.cmd('normal! ggzt')
local function position() return api.nvim_win_call(panel.transcript,vim.fn.winsaveview) end
local function same_view(before,label)
  local after=position()
  for _,field in ipairs({'lnum','col','topline','leftcol','skipcol'}) do
    assert(after[field]==before[field],label..' changed '..field..': '..vim.inspect({before=before,after=after}))
  end
end
local before=position()
local timing=session.current.view.projection.lines[2]
assert(vim.wait(1500,function() return session.current.view.projection.lines[2]~=timing end,20),
  'Live Activity timer did not refresh')
same_view(before,'Timer refresh while the tail is visible')
vim.cmd('redraw!')
local live_pos=api.nvim_win_get_position(panel.transcript)
local shown=screen_row(live_pos[1]+3,vim.o.columns,live_pos[2]+1)
local expected=session.current.view.projection.lines[2]:gsub('▾','▸',1)
assert(shown:sub(1,#expected)==expected,'Closed native fold did not display its updated Activity timer: '..shown)
vim.cmd('normal! 2j')
before=position()
answer.content=answer.content..'\nAnother line'
session:changed()
same_view(before,'New content while reading the transcript')
vim.cmd('normal! k')
before=position()
session:changed()
same_view(before,'Moving back up during generation')
-- Scrolling within one wrapped line also counts as reading, not following.
answer.content=string.rep('word ',300)
session:changed()
vim.cmd('normal! G$2gk')
before=position()
answer.content=answer.content..' more generated text'
session:changed()
same_view(before,'Wrapped-line navigation during generation')
-- The composer still follows new output when the transcript was left at its end.
vim.cmd('redraw!')
vim.cmd('normal! G$')
vim.cmd('redraw!')
api.nvim_set_current_win(panel.input)
session.current.records[#session.current.records+1]={id='tail-output',role='assistant',content='New output\nLast row'}
session:changed()
assert(api.nvim_win_get_cursor(panel.transcript)[1]==#session.current.view.projection.lines,'Composer lost automatic tail following')
assert(api.nvim_get_current_win()==panel.input,'Following new output stole composer focus')
api.nvim_win_call(panel.transcript,function() vim.cmd('normal! ggzt') end)
before=position()
session.current.records[#session.current.records+1]={id='more-output',role='assistant',content='More output'}
session:changed()
same_view(before,'Background output after scrolling away from the tail')
running._started_at=nil
session:changed()
local tick=api.nvim_buf_get_changedtick(session.current.view.transcript)
panel:render()
assert(api.nvim_buf_get_changedtick(session.current.view.transcript)==tick,'Unchanged refresh rewrote the transcript')
vim.fn.mkdir('build-ai/neovim','p')
vim.fn.writefile(snapshots,'build-ai/neovim/chat-layout.txt')
panel:hide()
vim.o.lines=original.lines
vim.o.columns=original.columns
vim.go.fillchars=original.fillchars
