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
assert(#projection.spans==3,'An empty Activity must remain visible')
assert(projection.spans[1].key=='activity:empty' and not projection.spans[1].group,
  'An empty Activity must not offer a bogus lazy-detail request')
local groups={}
for _, mark in ipairs(projection.highlights) do groups[mark.group]=true end
for _, name in ipairs({'SosHeading','SosBold','SosCode','SosActivity','SosTool'}) do
  assert(groups[name], 'Missing semantic highlight '..name)
end
assert(not groups.SosAssistant)
assert(projection.lines[1]=='   Hello' and projection.bands[1]=='SosSane')
for _,text in ipairs(projection.lines) do assert(text~='You' and text~='SOS') end
local layout=Render.project({mode='slop',records={
  {id='u1',role='user',metadata={mode='sane'},content='First\nSecond'},
  {id='a1',role='assistant',content='Reply'},
  {id='t1',role='activity',count=1},
  {id='a2',role='assistant',content='More'},
  {id='t2',role='activity',count=1},
  {id='u2',role='user',content='Next'},
  {id='t3',role='activity',count=1},
}},{},20)
assert(layout.bands[1]=='SosSane' and layout.bands[2]=='SosSane')
assert(not layout.bands[layout.anchors['message:a1:1']])
assert(layout.bands[layout.anchors['message:u2:1']]=='SosSlop')
local separators=0
for _,text in ipairs(layout.lines) do if text==string.rep('─',20) then separators=separators+1 end end
assert(separators==1,'Only subsequent Activities within the same turn need a separator')
local planned=Render.project({records={{id='p',objective_plan=true,content='Plan'}}},{},20)
assert(planned.lines[1]==string.rep('─',20),'Objective drawer separator is missing')
assert(not next(planned.bands),'Objective text must not have a side stripe')
assert(planned.spans[1].highlight=='SosObjectivePlan')
local original_min=vim.o.foldminlines
vim.o.foldminlines=20
local panel=Panel.new(session)
panel:show()
assert(vim.wo[panel.transcript].foldminlines==0,'Inherited foldminlines hides short Activity folds')
assert(not vim.wo[panel.transcript].winhighlight:find('SosNormal',1,true))
assert(vim.wo[panel.transcript].statuscolumn=='' and vim.wo[panel.transcript].foldcolumn=='0')
local band
for _,mark in ipairs(api.nvim_buf_get_extmarks(session.current.view.transcript,Highlights.namespace,0,-1,{details=true})) do
  if mark[4].virt_text then band=mark[4]; break end
end
assert(band and band.virt_text[1][1]=='█' and band.virt_text_win_col==1 and band.virt_text_repeat_linebreak)
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
assert(session.current.view.projection.lines[session.current.view.projection.anchors[key]]:match('^   ▾ Activity'))
assert(api.nvim_get_hl(0,{name='SosActivity'}).fg==0x71808E)
assert(api.nvim_get_hl(0,{name='SosObjectivePlan'}).fg==0x71808E)
assert(api.nvim_get_hl(0,{name='SosObjectivePlanUpdated'}).fg==0x05b07d)
assert(api.nvim_get_hl(0,{name='SosPlanBody',link=true}).link=='Normal')
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
api.nvim_set_hl(0,'SosSane',{fg='#123456'})
Highlights.setup()
assert(api.nvim_get_hl(0,{name='SosSane'}).fg==0x123456)
vim.cmd('colorscheme default')
Highlights.setup()
assert(api.nvim_get_hl(0,{name='SosSane'}).fg~=nil)
panel:hide()
vim.o.foldminlines=original_min
print('Semantic colors, visible empty Activities and native refolding tests passed')
