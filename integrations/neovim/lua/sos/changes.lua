local M = {}
M.__index=M

function M.blocks(diffs)
  local blocks={}
  for _,diff in ipairs(diffs or {}) do
    local hunk
    local index=0
    for line in ((diff.patch or '')..'\n'):gmatch('(.-)\n') do
      -- Counts are optional in unified headers.
      local old,oldcount,new,newcount=line:match('^@@ %-(%d+)(,?%d*) %+(%d+)(,?%d*) @@')
      if old then
        hunk={diff=diff,path=diff.path,hunk=index,old_start=tonumber(old),new_start=tonumber(new),
          old_count=tonumber((oldcount:gsub('^,',''))) or 1,new_count=tonumber((newcount:gsub('^,',''))) or 1,
          lines={}}
        index=index+1
        blocks[#blocks+1]=hunk
      elseif hunk then hunk.lines[#hunk.lines+1]=line end
    end
    if index==0 then blocks[#blocks+1]={diff=diff,path=diff.path,hunk=0,new_start=1,old_start=1,lines={}} end
  end
  for n,block in ipairs(blocks) do
    block.sequence=n
    block.order=(block.diff.hunk_order or {})[block.hunk+1]
    block.explanation=(block.diff.hunk_explanations or {})[block.hunk+1]
  end
  table.sort(blocks,function(a,b)
    if a.order~=nil and b.order~=nil and a.order~=b.order then return a.order<b.order end
    if (a.order~=nil)~=(b.order~=nil) then return a.order~=nil end
    return a.sequence<b.sequence
  end)
  return blocks
end

function M.new()
  return setmetatable({positions={},selections={},locations={}},M)
end
function M:bind(branch,state,index)
  local step=state and state.steps[index+1]
  if not step then self.step=nil; return end
  self.state,self.step,self.index,self.branch=state,step,index,branch
  self.key=table.concat({branch,step.identity,tostring(step.display_revision or 0),tostring(step.generation or 0)},'/')
  self.part=self.selections[self.key]
  if self.part==nil then
    self.part=0
    for n,part in ipairs(step.substeps) do
      if #(part.diffs or {})>0 then self.part=n-1; break end
    end
  end
  self.part=math.max(0,math.min(self.part,#step.substeps))
  self:refresh()
end
function M:refresh()
  if not self.step then return end
  self.selections[self.key]=self.part
  self.item=self.part==#self.step.substeps and self.step.whole or self.step.substeps[self.part+1]
  self.blocks=M.blocks(self.item.diffs)
  self.position_key=self.key..'/'..self.part
  self.change=math.max(0,math.min(self.positions[self.position_key] or 1,#self.blocks))
  if #self.blocks>0 then self.change=math.max(1,self.change) end
  local location=self.locations[self.position_key]
  if location then
    for n,block in ipairs(self.blocks) do
      if block.path==location.path and block.hunk==location.hunk then self.change=n; break end
    end
  end
  self:remember()
end
function M:substep(delta)
  if not self.step then return end
  self.part=math.max(0,math.min(self.part+delta,#self.step.substeps))
  self:refresh()
end
function M:move(delta)
  if not self.step then return end
  self.change=#self.blocks==0 and 0 or math.max(1,math.min(self.change+delta,#self.blocks))
  self:remember()
end
function M:remember()
  self.positions[self.position_key]=self.change
  local block=self:selected()
  self.locations[self.position_key]=block and {path=block.path,hunk=block.hunk} or nil
end
function M:selected() return self.blocks and self.blocks[self.change] end
function M:heading()
  if not self.step then return 'No preserved result' end
  local whole=self.part==#self.step.substeps
  local planned=self.state.plan.steps[self.index+1]
  local part=planned.substeps[self.part+1]
  local title=whole and planned.title or (type(part)=='table' and part.title or part) or ''
  return 'Step '..(self.index+1)..'/'..#self.state.steps..' | '..(whole and 'Whole step' or 'Substep '..(self.part+1)..'/'..#self.step.substeps)
    ..' | Diff '..self.change..'/'..#self.blocks..' | '..title:gsub('[\r\n]',' ')
end
function M:explanation()
  local selected=self:selected()
  -- Empty boundaries have no hunk; their preserved explanation is still useful.
  local explanation=selected and selected.explanation or (not selected and self.item and self.item.explanation)
  return vim.split(explanation and explanation:match('%S') and explanation or 'No explanation available for this change.','\n',{plain=true})
end
return M
