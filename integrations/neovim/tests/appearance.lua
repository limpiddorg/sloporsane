local api=vim.api
local Render=require('sos.render')
local Highlights=require('sos.highlights')
local Session=require('sos.session')
local Panel=require('sos.panel')
local client={transport={state='ready',pending={}},conversation={},operations={}}
function client:request() error('Appearance must not eagerly load historical details') end
local session=Session.new(client,{workspace=vim.fn.getcwd()})
local records={
  {id='u',role='user',content='Hello'},
  {id='empty',role='activity',count=0},
  {id='a',role='assistant',content='# Heading\n**Important** and `code`\n```lua\nreturn true\n```'},
  {id='tools',role='activity',count=1,activities={
    {live=true,kind='tool',activityIndex=0,toolName='bash',arguments={mainCommand='echo'},result={output='safe',isError=false}},
  }},
}
session:history():install(records,{})
local projection=Render.project(session.current,{})
assert(#projection.spans==2,'Empty Activity must not create a drawer')
local groups={}
for _, mark in ipairs(projection.highlights) do groups[mark.group]=true end
for _, name in ipairs({'SosUser','SosAssistant','SosSeparator','SosHeading','SosBold','SosCode','SosActivity','SosDetail'}) do
  assert(groups[name], 'Missing semantic highlight '..name)
end
local original_min=vim.o.foldminlines
vim.o.foldminlines=20
local panel=Panel.new(session)
panel:show()
assert(vim.wo[panel.transcript].foldminlines==0,'Inherited foldminlines hides short Activity folds')
local function folded(key)
  return api.nvim_win_call(panel.transcript,function()
    for _, span in ipairs(session.current.view.projection.spans) do
      if span.key==key then return vim.fn.foldclosed(span.start) end
    end
    error('Missing fold '..key)
  end)
end
local function command(key, text)
  api.nvim_win_call(panel.transcript,function()
    for _, span in ipairs(session.current.view.projection.spans) do
      if span.key==key then
        api.nvim_win_set_cursor(panel.transcript,{span.start,0})
        vim.cmd('normal '..text)
        return
      end
    end
  end)
end
local key='activity:tools'
assert(folded(key)>0)
command(key,'zo')
assert(folded(key)==-1)
command(key..'/item/0','zo')
assert(folded(key..'/item/0')==-1)
command(key..'/item/0','zc')
assert(folded(key..'/item/0')>0)
command(key,'zc')
assert(folded(key)>0)
session:changed()
assert(folded(key)>0,'Rendering reopened an explicitly closed Activity')
command(key,'zo')
assert(folded(key)==-1)
panel:hide(); panel:show()
assert(folded(key)==-1,'Reopening panel lost native fold state')
assert(#api.nvim_buf_get_extmarks(session.current.view.transcript,Highlights.namespace,0,-1,{})>0)
-- Colorscheme overrides are preserved by default definitions and theme reloads.
api.nvim_set_hl(0,'SosUser',{fg='#123456'})
Highlights.setup()
assert(api.nvim_get_hl(0,{name='SosUser'}).fg==0x123456)
vim.cmd('colorscheme default')
assert(api.nvim_get_hl(0,{name='SosUser'}).fg~=nil)
panel:hide()
vim.o.foldminlines=original_min
print('Semantic colors, empty Activities and native refolding tests passed')
