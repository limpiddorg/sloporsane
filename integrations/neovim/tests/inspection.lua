local Changes=require('sos.changes')
local Source=require('sos.inspection_source')
local Session=require('sos.session')
local root=vim.fn.getcwd()..'/build-ai/neovim/inspection'
vim.fn.mkdir(root,'p')
local path=root..'/example.lua'
local after='one\nnew\nthree\nfour\nfive\nsix\nchanged\n'
local diff={path='example.lua',before='one\nold\nthree\nfour\nfive\nsix\nold\n',after=after,
  patch='--- a/example.lua\n+++ b/example.lua\n@@ -1,3 +1,3 @@\n one\n-old\n+new\n three\n@@ -6,2 +6,2 @@\n six\n-old\n+changed\n'}
local state={status='waiting',plan={steps={{title='Build',substeps={{title='Edit'},{title='Explain only'}}}}},steps={
  {identity='stable',generation=1,display_revision=0,substeps={
    {complete=true,explanation='Edit two regions',diffs={diff}},
    {complete=true,explanation='Nothing to edit',diffs={}}},whole={complete=true,explanation='Whole result',diffs={diff}}}}}
local model=Changes.new()
model:bind('branch',state,0)
assert(#model.blocks==2 and model.blocks[2].hunk==1)
model:move(1); assert(model.change==2 and model:selected().new_start==6)
model:move(20); assert(model.change==2)
model:substep(1); assert(model.change==0 and model:heading():find('Substep 2/2'))
assert(table.concat(model:explanation(),'\n'):find('Nothing to edit\nNo changes'))
model:substep(-1); assert(model.change==2)
model:substep(-10); assert(model.part==0)
model:substep(20); assert(model.part==2 and model:heading():find('Whole step'))
local revised=vim.deepcopy(state); revised.steps[1].display_revision=1
model:bind('branch',revised,0); assert(model.part==0 and model.change==1)
model:bind('branch',state,0); assert(model.part==2)
model:substep(-2); assert(model.change==2)
local short=Changes.blocks({{path='x',patch='@@ -2 +4 @@\n-old\n+new'}})
assert(short[1].old_count==1 and short[1].new_start==4)

local function bytes(file,text)
  local fd=assert(vim.uv.fs_open(file,'w',384)); assert(vim.uv.fs_write(fd,text,0)); vim.uv.fs_close(fd)
end
bytes(path,after)
assert(Source.resolve(diff,root).kind=='live')
assert(Source.resolve(diff,root,true).kind=='historical')
local absent=vim.tbl_extend('force',diff,{path='absent.lua'})
assert(Source.resolve(absent,root).kind=='historical')
local deleted={path='deleted.lua',before='deleted text\n',after='',patch='deleted file mode 100644\n--- a/deleted.lua\n+++ /dev/null\n@@ -1 +0,0 @@\n-deleted text'}
assert(Source.resolve(deleted,root).kind=='deleted' and Source.resolve(deleted,root).text=='deleted text\n')
local emptied={path='empty.lua',before='Before truncation\n',after='',patch='@@ -1 +0,0 @@\n-Before truncation'}
assert(Source.resolve(emptied,root).side=='before' and Source.resolve(emptied,root).kind=='empty_after')
local binary={path='image.bin',before='',after='',patch='GIT binary patch\nliteral 2'}
assert(Source.resolve(binary,root).kind=='binary')
local added={path='added.lua',before='',after='added\n',patch='new file mode 100644\n--- /dev/null\n+++ b/added.lua\n@@ -0,0 +1 @@\n+added'}
bytes(root..'/added.lua',added.after)
assert(Source.resolve(added,root).added and Source.resolve(added,root).kind=='live')
local outside=root..'-outside.lua'; bytes(outside,'outside\n')
vim.uv.fs_unlink(root..'/link.lua'); assert(vim.uv.fs_symlink(outside,root..'/link.lua'))
assert(Source.resolve(vim.tbl_extend('force',diff,{path='link.lua'}),root).kind=='historical')
assert(not Source.path(root,'../inspection-outside.lua'))
local user=vim.api.nvim_create_buf(true,false)
vim.api.nvim_buf_set_name(user,path)
vim.api.nvim_buf_set_lines(user,0,-1,false,{'Unrelated unsaved work'})
vim.bo[user].modified=true
local flags={vim.bo[user].modifiable,vim.bo[user].readonly,vim.bo[user].modified}
assert(Source.resolve(diff,root).kind=='historical')
local client={transport={state='ready'},conversation={},sent={}}
function client:request(frame) self.sent[#self.sent+1]=frame; return tostring(#self.sent) end
local s=Session.new(client,{workspace=root})
s.current.id='branch'; s.current.kind='step'
s.workflow.branch_id='branch'; s.workflow.selected_identity='stable'
s.workflow.state=state; s.workflow.info={execution_workspace=root,workspace='/original'}
local cwd=vim.fn.getcwd()
local origin=vim.api.nvim_get_current_win()
vim.api.nvim_win_set_buf(origin,user)
s.panel_origin=origin
s:inspect()
local inspect=s.inspector
assert(vim.api.nvim_get_current_win()==origin and vim.api.nvim_win_get_buf(origin)==user)
assert(inspect.source.kind=='historical' and inspect.source.text==after)
assert(not vim.bo[inspect.code_buffer].modifiable and vim.bo[inspect.code_buffer].readonly)
assert(vim.api.nvim_buf_get_lines(user,0,-1,false)[1]=='Unrelated unsaved work')
assert(vim.bo[user].modifiable==flags[1] and vim.bo[user].readonly==flags[2] and vim.bo[user].modified==flags[3])
assert(#vim.api.nvim_buf_get_extmarks(inspect.code_buffer,inspect.namespace,0,-1,{})>0)
s:next_change()
assert(inspect.model.change==2 and vim.api.nvim_win_get_cursor(inspect.code)[1]==7)
assert(vim.wo[inspect.code].winbar:find('Change 2/2'))
assert(table.concat(vim.api.nvim_buf_get_lines(inspect.explanation_buffer,0,-1,false),'\n'):find('Change 2/2'))
s:next_change(); assert(inspect.model.change==2)
s:next_substep(); assert(inspect.model.part==1 and inspect.source.kind=='empty')
s:next_change(); assert(inspect.model.part==1 and inspect.model.change==0)
s:previous_substep(); assert(inspect.model.change==2)
inspect:toggle_explanation(); assert(inspect.collapsed)
inspect:toggle_explanation(); assert(vim.api.nvim_win_is_valid(inspect.explanation))
-- Removing the conflicting buffer makes a future explicit navigation live-safe.
vim.api.nvim_win_set_buf(origin,vim.api.nvim_create_buf(true,false))
vim.api.nvim_buf_delete(user,{force=true})
s:previous_change(); assert(inspect.source.kind=='live')
bytes(path,'Successor contents\n'); inspect:recheck()
assert(inspect.source.kind=='historical' and inspect.source.text==after)
assert(not table.concat(vim.api.nvim_buf_get_lines(inspect.code_buffer,0,-1,false),'\n'):find('Successor'))
bytes(path,after); s:next_change(); assert(inspect.source.kind=='live')
vim.uv.fs_unlink(path); inspect:recheck(); assert(inspect.source.kind=='historical')
bytes(path,after)
s:previous_change(); assert(inspect.source.kind=='live')
vim.uv.fs_unlink(path); assert(vim.uv.fs_symlink(outside,path))
inspect:recheck(); assert(inspect.source.kind=='historical')
vim.uv.fs_unlink(path); bytes(path,after)
s:next_change(); assert(inspect.source.kind=='live')
s.workflow.state=vim.deepcopy(state)
s.workflow.state.steps[1].substeps[1].diffs[1].after='Different preserved version\n'
s:changed(); assert(inspect.source.kind=='historical' and inspect.source.text=='Different preserved version\n')
assert(vim.fn.getcwd()==cwd and #client.sent==0,'Inspection changed cwd or sent daemon operations')
assert(table.concat(vim.fn.readfile(path),'\n')..'\n'==after,'Inspection mutated the isolated file')
local replacement=vim.api.nvim_create_buf(true,false)
vim.api.nvim_buf_set_lines(replacement,0,-1,false,{'Keep this code window'})
vim.api.nvim_win_set_buf(inspect.code,replacement)
local reused=inspect.code
inspect:hide()
assert(vim.api.nvim_win_is_valid(reused) and vim.api.nvim_win_get_buf(reused)==replacement)
print('Preserved change navigation/source/window tests passed')
