-- Chat splits follow native resizing while preserving drafts and composer metadata.
local api=vim.api
local Session=require('sos.session')
local Panel=require('sos.panel')
local original={lines=vim.o.lines,columns=vim.o.columns,laststatus=vim.o.laststatus}
-- Match the allocated headless grid; attached-UI sizing is tested in chat_layout.lua.
vim.o.lines=24
vim.o.columns=80
local client={transport={state='ready',pending={}},conversation={},operations={}}
function client:request() error('Composer layout must not make daemon requests') end
local s=Session.new(client,{workspace='/original',progress=false})
s.info={model='gpt',reasoning='high',last_turn_used_context=1250,total_context=128000}
local panel=Panel.new(s)
local tab=api.nvim_get_hl(0,{name='SosTabActive'})
local version=api.nvim_get_hl(0,{name='SosVersion'})
assert(tab.fg==0xE6EDF3 and tab.bg==0x277BE3 and tab.bold,'Active context tab must match the TUI colors')
assert(version.fg==0x71808E and not version.bold and not version.bg,'Version must be muted and non-bold, without a blue background')
assert(vim.deep_equal(require('sos.render').project({records={}},{}).lines,{''}),'Empty conversations must not have a welcome label')
for _,mode in ipairs({'Sane','Slop'}) do
  local badge=api.nvim_get_hl(0,{name='SosMode'..mode})
  assert(badge.fg==0 and badge.bg==(mode=='Sane' and 0x05b07d or 0xf51d53) and badge.bold,
    'Mode badges must use bold black text on the mode background')
  assert(not api.nvim_get_hl(0,{name='Sos'..mode}).bg,'Badge styling leaked into transcript mode colors')
end
local function header(input)
  return api.nvim_eval_statusline(vim.wo[input].winbar,{winid=input,maxwidth=api.nvim_win_get_width(input)}).str
end
local function check_header(input,mode)
  local text=header(input)
  assert(text:find('gpt',1,true) and text:find('[1%]',1,true),text)
  if api.nvim_win_get_width(input)>=32 then assert(text:find('gpt | high',1,true),text) end
  if api.nvim_win_get_width(input)>=48 then assert(text:find('1 250 / 128 000 [1%]',1,true),text) end
  local badge='     '..(mode or 'Sane')..'      '
  assert(text:sub(-15)==badge,text)
  assert(vim.wo[input].winbar:find('%#SosMode'..(mode or 'Sane')..'#'..badge..'%*',1,true),
    'Badge padding or highlight reset is missing')
  assert(not text:find('Your message',1,true))
end
local origin=api.nvim_get_current_win()
local discussion=s.current
for _,laststatus in ipairs({2,3}) do
  vim.o.laststatus=laststatus
  s.current.mode='sane'
  panel:show()
  assert(api.nvim_get_current_win()==origin,'Opening chat stole focus')
  local input=panel.input
  assert(api.nvim_win_get_height(input)==5,'Opening must give the composer five lines')
  assert(vim.wo[panel.transcript].winbar:find('%#SosTabActive# Discussion %#SosNormal#',1,true))
  assert(vim.wo[panel.transcript].winbar:find('%#SosVersion#',1,true))
  for _,window in ipairs({input,panel.transcript}) do
    assert(not vim.wo[window].winfixheight and not vim.wo[window].winfixwidth,'Chat split dimensions are locked')
  end
  assert(vim.wo[input].statusline==' ' and vim.wo[panel.transcript].statusline==' ')
  assert(vim.wo[input].foldcolumn=='0' and not vim.wo[input].foldenable and vim.wo[input].statuscolumn=='')
  assert(not vim.wo[input].number and not vim.wo[input].relativenumber and vim.wo[input].signcolumn=='no')
  assert(not vim.wo[input].winhighlight:find('Sos',1,true))
  check_header(input)
  assert(not vim.wo[input].winbar:find('Original:',1,true))
  api.nvim_set_current_win(input)
  api.nvim_win_set_width(input,25)
  api.nvim_win_set_height(input,3)
  vim.cmd('wincmd =')
  assert(api.nvim_win_get_width(input)>25 and api.nvim_win_get_height(input)>3,
    'Chat splits resisted normal window balancing')
  for _,buffer in ipairs({s.current.view.input,s.current.view.transcript}) do
    for _,mode in ipairs({'n','i'}) do
      for _,key in ipairs({'<Tab>','<S-Tab>'}) do
        local mapping=api.nvim_buf_call(buffer,function() return vim.fn.maparg(key,mode,false,true) end)
        if buffer==s.current.view.input and key=='<Tab>' then
          assert(mapping.buffer==1 and mapping.desc=='Toggle SOS Sane/Slop mode','Composer Tab must toggle Sane/Slop')
        else assert(mapping.buffer~=1,'SOS replaced a transcript or Shift-Tab mapping') end
      end
    end
  end
  api.nvim_set_current_win(panel.transcript)
  vim.fn.maparg('i','n',false,true).callback()
  assert(api.nvim_get_current_win()==input)
  check_header(input)
  s.current.mode='slop'; s:changed()
  check_header(input,'Slop')
  assert(vim.wo[input].winbar:find('SosModeSlop',1,true))
  local draft={}
  for n=1,20 do draft[n]='Line '..n..' of the message' end
  api.nvim_buf_set_lines(s.current.view.input,0,-1,false,draft)
  vim.cmd('normal! G')
  vim.cmd('redraw')
  local cursor=api.nvim_win_get_cursor(input)
  assert(vim.fn.winsaveview().topline>1,'A long draft did not scroll inside the field')
  vim.cmd('wincmd =')
  api.nvim_win_set_height(input,7)
  local height=api.nvim_win_get_height(input)
  local width=api.nvim_win_get_width(input)
  api.nvim_win_set_width(input,math.max(30,width-10))
  api.nvim_exec_autocmds('WinResized',{modeline=false})
  assert(api.nvim_win_get_height(input)==height,'Render overrode the chosen composer height')
  s.current.records={{id='reply',role='assistant',content='Incoming answer'}}
  s:changed()
  assert(api.nvim_get_current_win()==input and api.nvim_win_get_cursor(input)[1]==cursor[1])
  assert(api.nvim_win_get_height(input)==height and #api.nvim_buf_get_lines(s.current.view.input,0,-1,false)==20)
  api.nvim_set_current_win(origin)
  check_header(input,'Slop')
  panel:hide(); panel:show()
  assert(not vim.wo[panel.input].winfixheight and s.current.draft==table.concat(draft,'\n'))
  assert(api.nvim_win_get_height(panel.input)==5,'Reopening must restore the default five-line composer')
  assert(vim.o.laststatus==laststatus,'Composer styling changed a global editor option')
  panel:hide()
end
panel:show()
local input=panel.input
api.nvim_win_set_width(input,60)
panel:update_input()
local context='1 250 / 128 000 [1%]'
assert(header(input):find(context,1,true)==math.floor((60-vim.fn.strdisplaywidth(context))/2)+1,
  'Context usage is not centered')
for _,width in ipairs({30,20,10,8}) do
  api.nvim_win_set_width(input,width)
  panel:update_input()
  local text=header(input)
  local badge_width=math.min(15,width-1)
  local padding=math.floor((badge_width-4)/2)
  local badge=string.rep(' ',padding)..'Slop'..string.rep(' ',badge_width-padding-4)
  assert(vim.fn.strdisplaywidth(text)==width and text:sub(-badge_width)==badge,text)
  assert(not text:find('128 000',1,true),'Context counts were not hidden first')
  if width==30 then assert(text:find('gpt',1,true) and not text:find('high',1,true) and text:find('[1%]',1,true),text) end
  if width==20 then assert(text:find('gpt',1,true) and not text:find('high',1,true),text) end
end
api.nvim_win_set_width(input,70)
s.info={model='x%#Error#\nmodel',reasoning='none',last_turn_used_context=vim.NIL,total_context=65536}
s:changed()
assert(header(input):find('x%#Error# model | No reasoning',1,true),header(input))
assert(header(input):find('— / 65 536 [—]',1,true),header(input))
s.info.last_turn_used_context=0
s:changed()
assert(header(input):find('0 / 65 536 [0%]',1,true),header(input))
s.info={model='gpt',reasoning='high',last_turn_used_context=vim.NIL,total_context=vim.NIL}
s:changed()
assert(header(input):find('-',1,true) and not header(input):find(' / ',1,true),header(input))
s.info={model='gpt',reasoning='high',last_turn_used_context=1250,total_context=128000}
s:changed()
s:event({type='reasoning.updated',info={model='gpt',reasoning='medium',last_turn_used_context=12800,total_context=128000}})
assert(header(input):find('gpt | medium',1,true) and header(input):find('[10%]',1,true),header(input))
s.info={model='gpt',reasoning='high',last_turn_used_context=1250,total_context=128000}
s:changed()
panel:hide()
panel:show()
input=panel.input
api.nvim_win_set_height(input,7)
local height=api.nvim_win_get_height(input)
s.workflow.branch_id='branch'
s.workflow.state={status='waiting',review_step=0,steps={{identity='step',generation=1,whole={complete=true,reviewed=false},substeps={}}}}
local review={id='branch',kind='review',context_key='review',review_key=s.workflow:review_key(0),
  step=0,substep=0,mode='sane',draft='',records={},workspace='/original'}
s.workflow:select(review)
assert(panel.input==input and api.nvim_win_get_height(input)==height)
check_header(input)
review.archived=true; s:changed()
assert(panel.input==input and api.nvim_win_is_valid(input),'Read-only review lost its status input')
assert(table.concat(api.nvim_buf_get_lines(review.view.input,0,-1,false),'\n'):find('Archived review',1,true))
assert(not vim.bo[review.view.input].modifiable)
s.workflow:select(discussion)
input=panel.input
assert(api.nvim_win_get_height(input)==height and #api.nvim_buf_get_lines(discussion.view.input,0,-1,false)==20)
panel:hide()
-- Closing either split closes its partner, without discarding the draft.
for _,part in ipairs({'input','transcript'}) do
  panel:show()
  local input_win,transcript_win=panel.input,panel.transcript
  api.nvim_set_current_win(panel[part])
  vim.cmd('close')
  assert(vim.wait(1000,function() return panel.input==nil and panel.transcript==nil end))
  assert(not api.nvim_win_is_valid(input_win) and not api.nvim_win_is_valid(transcript_win))
  assert(discussion.draft:find('Line 20',1,true),'Closing a split lost its draft')
end
panel:show()
-- Read the identity beside the loaded plugin, not from another runtimepath entry.
local fixture=vim.fn.getcwd()..'/build-ai/neovim/version-fixture'
vim.fn.mkdir(fixture..'/lua/sos','p')
vim.fn.writefile(vim.fn.readfile('integrations/neovim/lua/sos/panel.lua'),fixture..'/lua/sos/panel.lua')
for _,value in ipairs({'missing','dev','1.2.3','v1.2.3','v1.2.3-rc.1','%#Error#bad'}) do
  if value=='missing' then vim.fn.delete(fixture..'/VERSION')
  else vim.fn.writefile({value},fixture..'/VERSION') end
  local versioned=assert(loadfile(fixture..'/lua/sos/panel.lua'))()
  versioned.update_status(panel)
  local expected=({['1.2.3']='v1.2.3',['v1.2.3']='v1.2.3',['v1.2.3-rc.1']='v1.2.3-rc.1'})[value] or 'dev'
  local text=header(panel.transcript)
  assert(text==' Discussion'..string.rep(' ',api.nvim_win_get_width(panel.transcript)-12-#expected)..expected..' ',text)
end
panel:update_status()
api.nvim_set_current_win(panel.transcript)
vim.fn.maparg('q','n',false,true).callback()
assert(panel.input==nil and panel.transcript==nil)
vim.o.lines=original.lines
vim.o.columns=original.columns
vim.o.laststatus=original.laststatus
print('Native resizable chat splits, responsive model/context/mode header and draft preservation passed')
