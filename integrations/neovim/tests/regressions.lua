local api=vim.api
local Session=require('sos.session')
local Source=require('sos.inspection_source')
local Changes=require('sos.changes')
local Render=require('sos.render')
local Panel=require('sos.panel')
local root=vim.fn.getcwd()..'/build-ai/neovim/regressions'
vim.fn.mkdir(root,'p')
local function write(path,text)
  local fd=assert(vim.uv.fs_open(path,'w',384))
  assert(vim.uv.fs_write(fd,text,0)); vim.uv.fs_close(fd)
end
local client={transport={state='ready',pending={}},conversation={},operations={},sent={}}
function client:request(frame,terminal,callback)
  self.sent[#self.sent+1]={frame=frame,terminal=terminal,callback=callback}
  return tostring(#self.sent)
end
local s=Session.new(client,{workspace=root,progress=false})
s.configured=true
local w=s.workflow
w.branch_id='regression'; w.discussion_id='discussion'; w.info={workspace=root,execution_workspace=root}
local diff={path='native.lua',before='local old = true\nreturn old\n',after='local value = true\nreturn value\n',
  patch='--- a/native.lua\n+++ b/native.lua\n@@ -1,2 +1,2 @@\n-local old = true\n-return old\n+local value = true\n+return value\n',
  hunk_explanations={'Rename the value without changing behavior'}}
write(root..'/native.lua',diff.after)
w.state={status='waiting',revision=1,work_step=0,review_step=0,
  plan={steps={{title='Native inspection',objective='Read actual isolated code',substeps={{title='Rename'}}}}},
  steps={{identity='native',generation=1,whole={complete=true,reviewed=false,diffs={diff}},
    substeps={{complete=true,explanation='Rename summary',diffs={diff},activity={
      {id='tool',role='assistant',content={{type='tool_call',callId='write',toolName='write',arguments={filePath='native.lua',content=diff.after}}}},
      {id='result',role='tool',content={{type='tool_result',callId='write',output='Written',isError=false}}},
    }}},validation={complete=true,started=true,activity={
      {id='check',role='assistant',content={{type='tool_call',callId='check',toolName='bash',arguments={mainCommand='false',command='false'}}}},
      {id='checked',role='tool',content={{type='tool_result',callId='check',output='failed check',isError=true,metadata={exit=1}}}},
    }}}}}
w:select_step('native')
local construction={id=w.branch_id,context_key='construction-evidence',records={}}
s:history(construction):install(require('sos.construction').records(w,construction,0,false),{})
local projected=Render.project(construction,w.info)
local text=table.concat(projected.lines,'\n')
assert(text:find('Validating the step') and text:find('Exit code 1') and text:find('failed check'))
assert(not text:find('filePath ='), 'Tool arguments leaked Lua table presentation')
local count=#client.sent
for _,record in ipairs(construction.records) do
  if record.role=='activity' then
    s:history(construction):load(record.id)
    for _,item in ipairs(record.activities) do s:history(construction):load(record.id,item.activityIndex) end
  end
end
assert(#client.sent==count,'Preserved construction Activity queried principal history')
s:inspect()
local inspector=s.inspector
local file=inspector.code_buffer
assert(inspector.source.kind=='live')
assert(vim.bo[file].buftype=='nofile' and vim.fn.fnamemodify(api.nvim_buf_get_name(file),':t')=='sos-inspection')
assert(not vim.bo[file].buflisted and vim.bo[file].filetype=='sos_inspection')
assert(vim.bo[file].readonly and not vim.bo[file].modifiable and not vim.bo[file].modeline)
assert(not pcall(api.nvim_buf_set_lines,file,0,1,false,{'accidental edit'}))
local deleted={}
for _,mark in ipairs(api.nvim_buf_get_extmarks(file,inspector.namespace,0,-1,{details=true})) do
  for _,line in ipairs(mark[4].virt_lines or {}) do deleted[#deleted+1]=line[1][1] end
end
assert(table.concat(deleted,'\n'):find('local old = true') and table.concat(deleted,'\n'):find('return old'))
s:before()
assert(inspector.source.side=='before' and vim.bo[inspector.code_buffer].buftype=='nofile')
assert(table.concat(api.nvim_buf_get_lines(inspector.code_buffer,0,-1,false),'\n')==diff.before:sub(1,-2))
s:before(); assert(inspector.code_buffer==file and vim.bo[file].buftype=='nofile')
assert(not vim.bo[inspector.code_buffer].autoread)
write(root..'/native.lua','Successor code that must not replace reviewed evidence\n')
vim.cmd('checktime '..inspector.code_buffer)
assert(vim.wait(1500,function() return inspector.source.kind=='historical' end,10))
assert(table.concat(api.nvim_buf_get_lines(inspector.code_buffer,0,-1,false),'\n')==diff.after:sub(1,-2))
write(root..'/native.lua',diff.after)
s:next_change()
assert(inspector.source.kind=='live')
assert(table.concat(api.nvim_buf_get_lines(inspector.explanation_buffer,0,-1,false),'\n'):find(diff.hunk_explanations[1],1,true))
s:review_question()
assert(s.current.kind=='review' and inspector.visible and api.nvim_win_is_valid(inspector.code),'Review closed its evidence')
local old=s.current
old.records={{id='old',role='assistant',content='Previous review'}}; old.draft='Retain this question'
local updated=vim.deepcopy(w.state); updated.steps[1].generation=2
s:event({type='implementation.updated',sessionId=w.branch_id,implementation=updated})
assert(w:readonly(old) and w:label(old):find('superseded'))
count=#client.sent
s:send(); s:indicator(); s:validate()
assert(#client.sent==count and old.draft=='Retain this question','Superseded review allowed an action')
s:review_question()
assert(s.current~=old and s.current.review_key==w:review_key(0) and #s.current.records==0)
assert(not s.current.question_context,'A stale hunk leaked into the new review')
assert(old.records[1].content=='Previous review' and old.draft=='Retain this question')
-- Moving to the latest review must also reconcile the code to that generation.
assert(inspector.model.key:find('/2$'))
local panel=Panel.new(s); panel:show()
w:select_step('native',true)
assert(not vim.bo[s.current.view.input].modifiable)
local plugin=require('sos')
local previous_session,previous_panel=plugin.session,plugin.panel
plugin.session=function() return s end
plugin.panel=function() return panel end
vim.cmd('runtime plugin/sos.lua')
s.current.draft='preserve this draft'
local requests=#client.sent
vim.cmd('SosReasoning')
assert(#client.sent==requests+1 and client.sent[#client.sent].frame.type=='reasoning.next')
vim.fn.maparg('<Plug>(SosReasoning)','n',false,true).callback()
assert(#client.sent==requests+2 and client.sent[#client.sent].frame.type=='reasoning.next')
assert(s.current.draft=='preserve this draft','Thinking controls consumed the draft')
local previous_select=require('sos.ui').select
local prompted=0
require('sos.ui').select=function(_,_,callback) prompted=prompted+1; callback(nil) end
s.permissions={p={tool='write',permissionId='p',details={},always={}}}
vim.cmd('SosPermissions')
assert(prompted==1 and s.permissions.p and not s.permission_prompt)
s.current.draft='/permissions'
vim.cmd('SosSend')
assert(prompted==2,'Read-only context blocked permission controls')
require('sos.ui').select=previous_select
plugin.session,plugin.panel=previous_session,previous_panel
-- Construction status belongs only to the composer, not the inspection.
s.options.progress=true
s:changed()
local focus=api.nvim_get_current_win()
local status=panel.build_surface
assert(status and api.nvim_win_is_valid(status.float))
panel:hide()
require('sos.progress').update(s)
assert(not panel.build_surface and not api.nvim_win_is_valid(status.float) and api.nvim_get_current_win()==focus)
assert(inspector.visible and api.nvim_win_is_valid(inspector.code),'Hiding chat closed the code')
inspector:hide()
require('sos.progress').close(s)

local textual={path='text.lua',before='',after='local BinaryTree = 120000\n',patch='@@ -0,0 +1 @@\n+local BinaryTree = 120000'}
assert(Source.resolve(textual,root).kind~='binary')
textual.patch='@@ -0,0 +1 @@\n+print("GIT binary patch")\n+print("+++ /dev/null")'
assert(Source.resolve(textual,root).kind~='binary' and Source.resolve(textual,root).kind~='deleted')
assert(Source.resolve({path='b',patch='Binary files a/b and b/b differ\n',before='',after=''},root).kind=='binary')
assert(Source.resolve({path='b',patch='Binary contents changed (preserved in snapshot blobs).\n',before='',after=''},root).kind=='binary')
local ordered=vim.deepcopy(diff)
ordered.patch='@@ -1 +1 @@\n-a\n+b\n@@ -9 +9 @@\n-c\n+d'
ordered.hunk_order={2,1}; ordered.hunk_explanations={'First region','Second region'}
local model=Changes.new()
model:bind('b',{plan={steps={{title='S',substeps={{title='P'}}}}},steps={{identity='s',substeps={{complete=true,diffs={ordered}}},whole={}}}},0)
assert(model:selected().hunk==1 and table.concat(model:explanation(),'\n'):find('Second region'))
model:move(1)
assert(model:selected().hunk==0 and table.concat(model:explanation(),'\n'):find('First region'))

local md=require('sos.markdown')
local source='# Heading\n**Important** and `code` and [Docs](https://example.com)\n> Quote\n```lua\nreturn true\n```\n| Name | Value |\n| --- | --- |\n| A | |'
local rows=md.project(source)
assert(rows[1].text=='Heading' and rows[1].group=='SosHeading')
assert(rows[2].text=='Important and code and Docs' and rows[2].links[1].url=='https://example.com')
assert(rows[3].text=='│ Quote' and rows[4].text=='  lua' and rows[5].text=='  return true')
assert(#rows[9].cells==2 and rows[9].cells[2]=='')
local objective={id='objective',content='Build the feature'}
local projection=Render.project({records={{id='answer',role='assistant',content='Answer'}}},
  {objective_plans={objective},objective_plan_turns={{message_id='answer',objective_id='objective'}}})
assert(next(projection.plan_lines) and not projection.actions)
for _,line in ipairs(projection.lines) do
  assert(not line:find('[ Implementation',1,true) and not line:find('[ Implement (Auto plan)',1,true),
    'Objective drawers must not contain implementation buttons')
end
local tools=require('sos.tool_format')
local title=tools.title({toolName='bash',arguments={mainCommand='pytest'},result={metadata={timedOut=true,permissions={{decision='ai_refused'}}}}})
assert(title:find('Failed') and title:find('AI'))
local failed_read=Render.project({records={{id='read',role='activity',count=1,activities={
  {live=true,activityIndex=0,toolName='read',arguments={filePath='missing'},result={isError=true,output='Missing file'}},
}}}}, {})
assert(table.concat(failed_read.lines,'\n'):find('Missing file'))
print('Review, permissions, isolated inspection, evidence, progress and presentation regressions passed')
