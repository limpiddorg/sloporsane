-- Exercise the keyboard-first inspection path without substep pickers or lists.
local api=vim.api
local Session=require('sos.session')
local root=vim.fn.getcwd()..'/build-ai/neovim/navigation'
vim.fn.mkdir(root,'p')
local alpha={path='alpha.lua',before='local alpha = 0\n\n\nreturn 0\n',after='local alpha = 1\n\n\nreturn alpha\n',
  patch='@@ -1 +1 @@\n-local alpha = 0\n+local alpha = 1\n@@ -4 +4 @@\n-return 0\n+return alpha\n',
  hunk_explanations={'Initialize alpha','Return the stored alpha'}}
local beta={path='beta.lua',before='return 0\n',after='return 2\n',patch='@@ -1 +1 @@\n-return 0\n+return 2\n',
  hunk_explanations={'Wire beta'}}
vim.fn.writefile(vim.split(alpha.after:sub(1,-2),'\n',{plain=true}),root..'/alpha.lua')
vim.fn.writefile({'return 2'},root..'/beta.lua')
local client={transport={state='ready',pending={}},conversation={},operations={},sent={}}
function client:request(frame) self.sent[#self.sent+1]=frame; return tostring(#self.sent) end
local s=Session.new(client,{workspace=root,progress=false})
local w=s.workflow
w.branch_id='navigation'; w.info={workspace=root,execution_workspace=root}
w.state={status='waiting',review_step=0,work_step=1,revision=1,plan={steps={
  {title='Build alpha',substeps={{title='Explain only'},{title='Store metadata'},{title='Wire behavior 100%'}}},
  {title='Build beta',substeps={{title='Beta'}}},
}},steps={
  {identity='alpha-step',generation=1,substeps={
    {complete=true,explanation='No changes needed',diffs={}},
    {complete=true,explanation='Store metadata',diffs={alpha}},
    {complete=true,explanation='Wire behavior',diffs={beta}},
  },whole={complete=true,reviewed=false,diffs={alpha,beta}}},
  {identity='beta-step',generation=1,substeps={{complete=true,diffs={beta}}},whole={complete=true,reviewed=false,diffs={beta}}},
}}
w.selected_identity='alpha-step'
local previous_select=require('sos.ui').select
require('sos.ui').select=function() error('Guided navigation opened an unnecessary picker') end
local laststatus=vim.o.laststatus
vim.o.laststatus=3
local function enter(menu)
  api.nvim_set_current_win(menu.window)
  vim.fn.maparg('<CR>','n',false,true).callback()
  assert(s.current.kind=='review' and s.inspector.visible,'Selecting a step did not open inspection beside its review')
end
local function select_step(menu,identity)
  for n,row in ipairs(menu.rows) do if row.key==identity then api.nvim_win_set_cursor(menu.window,{n,0}); return n end end
  error('Missing navigation row: '..identity)
end
local function key(mapping)
  api.nvim_set_current_win(s.inspector.code)
  vim.fn.maparg(mapping,'n',false,true).callback()
end
local function counters(fragment)
  local inspect=s.inspector
  assert(api.nvim_buf_get_lines(inspect.header_buffer,0,1,false)[1]:find(fragment,1,true),
    'Counters disappeared with a global statusline')
end
w:steps()
local menu=w.selector
assert(menu.rows[api.nvim_win_get_cursor(menu.window)[1]].key=='alpha-step','Selector did not highlight the selected step')
w:steps(); assert(w.selector==menu,'Reopening the selector created a second popup')
enter(menu)
local inspect=s.inspector
assert(inspect.model.part==1 and inspect.model.change==1,'First inspection did not open the first change')
assert(api.nvim_get_current_win()==inspect.code,'Step selection did not focus the code')
local count=#client.sent
key(']c')
assert(inspect.model.change==2 and api.nvim_win_get_cursor(inspect.code)[1]==4)
counters('Diff 2/2')
assert(table.concat(api.nvim_buf_get_lines(inspect.explanation_buffer,0,-1,false),'\n'):find('Return the stored alpha',1,true))
key(']s')
assert(inspect.model.part==2 and api.nvim_buf_get_lines(inspect.header_buffer,1,2,false)[1]=='beta.lua')
counters('Substep 3/3'); counters('Diff 1/1')
assert(api.nvim_buf_get_lines(inspect.header_buffer,0,1,false)[1]:find('Wire behavior 100%',1,true))
key('[s'); assert(inspect.model.part==1 and inspect.model.change==2)
key('[s'); assert(inspect.model.part==0 and inspect.model.change==0 and inspect.source.kind=='empty')
counters('Diff 0/0')
key(']s'); assert(inspect.model.part==1 and inspect.model.change==2)
assert(#client.sent==count,'Navigation sent a daemon request')
inspect:hide()
w:steps(); select_step(w.selector,'alpha-step')
enter(w.selector)
assert(inspect.model.part==1 and inspect.model.change==2,'Reopening lost the reading position')
w:steps()
local selected_row=select_step(w.selector,'beta-step')
-- Live state changes must not move the menu selection or editing focus.
local selected_window=w.selector.window
s:event({type='implementation.updated',sessionId=w.branch_id,implementation=vim.deepcopy(w.state)})
assert(api.nvim_get_current_win()==selected_window and api.nvim_win_get_cursor(selected_window)[1]==selected_row)
enter(w.selector)
assert(w.selected_identity=='beta-step' and inspect.model.part==0)
w:steps(); select_step(w.selector,'alpha-step'); enter(w.selector)
assert(inspect.model.part==1 and inspect.model.change==2,'Switching steps lost the prior substep/diff')
api.nvim_win_set_cursor(inspect.code,{2,0})
local progress=vim.deepcopy(w.state)
progress.steps[1].substeps[2].explanation='Updated construction explanation'
s:event({type='implementation.updated',sessionId=w.branch_id,implementation=progress})
assert(api.nvim_get_current_win()==inspect.code and api.nvim_win_get_cursor(inspect.code)[1]==2,'A live update moved the reader back to the hunk')
s:review_question()
assert(s.current.substep==1 and s.current.question_context.path=='alpha.lua' and s.current.question_context.hunk==1)
s.current.draft='Question about alpha'
count=#client.sent
key(']s')
assert(s.current.kind=='review' and s.current.substep==2 and s.current.question_context.path=='beta.lua')
key('[s')
assert(s.current.substep==1 and s.current.draft=='Question about alpha' and s.current.question_context.hunk==1)
assert(#client.sent==count,'Navigating review initiated a turn or requested state')
local plugin=require('sos')
local old_session,old_panel=plugin.session,plugin.panel
plugin.session=function() return s end
plugin.panel=function() error('Navigation reopened the chat') end
vim.cmd('runtime plugin/sos.lua')
vim.cmd('SosPreviousDiff'); assert(inspect.model.change==1 and s.current.question_context.hunk==0)
vim.cmd('SosNextDiff'); assert(inspect.model.change==2 and s.current.question_context.hunk==1)
plugin.session,plugin.panel=old_session,old_panel
assert(not w.state.steps[1].whole.reviewed and #client.sent==count)
assert(table.concat(vim.fn.readfile(root..'/alpha.lua'),'\n')==alpha.after:sub(1,-2))
inspect:hide()
if s.panel then s.panel:hide() end
require('sos.ui').select=previous_select
vim.o.laststatus=laststatus
print('Keyboard-first step/substep/diff navigation and persistent counters passed')
