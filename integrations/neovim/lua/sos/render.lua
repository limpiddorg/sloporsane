-- Pure transcript projection: no network requests, file reads or tool replay.
local M = {}
local Markdown = require('sos.markdown')
local Tools = require('sos.tool_format')
local projections = {}
function M.bind(buffer, projection)
  if not projections[buffer] then
    vim.api.nvim_buf_attach(buffer,false,{on_detach=function(_,detached) projections[detached]=nil end})
  end
  projections[buffer]=projection
end
function M.get(buffer) return projections[buffer] end
function M.text(record)
  if type(record.content) == 'string' then return record.content end
  local parts = {}
  for _, block in ipairs(record.content or {}) do
    if block.type == 'text' and type(block.text) == 'string' then table.insert(parts,block.text) end
  end
  return table.concat(parts,'\n')
end

function M.fold(lnum)
  local projection=projections[vim.api.nvim_get_current_buf()]
  return projection and projection.levels[lnum] or 0
end

function M.foldtext()
  local projection=projections[vim.api.nvim_get_current_buf()]
  return projection and projection.foldtext[vim.v.foldstart] or vim.fn.getline(vim.v.foldstart)
end

local function foldtext(projection,span)
  local text=projection.lines[span.start]:gsub('▾','▸',1)
  local marks={}
  for _,mark in ipairs(projection.highlights_by_line[span.start] or {}) do
    if mark.first then marks[#marks+1]=mark end
  end
  if projection.bands[span.start] then
    local col=projection.band_columns[span.start] or 1
    text=text:sub(1,col)..'█'..text:sub(col+2)
    for n,mark in ipairs(marks) do
      marks[n]=vim.tbl_extend('force',mark,{first=mark.first+2,last=mark.last+2})
    end
    marks[#marks+1]={first=col,last=col+#'█',group=projection.bands[span.start]}
  end
  table.sort(marks,function(a,b) return a.first<b.first end)
  local chunks,cursor={},0
  for _,mark in ipairs(marks) do
    if mark.first>=cursor then
      if mark.first>cursor then chunks[#chunks+1]={text:sub(cursor+1,mark.first),span.highlight} end
      chunks[#chunks+1]={text:sub(mark.first+1,mark.last),mark.group}
      cursor=mark.last
    end
  end
  if cursor<#text then chunks[#chunks+1]={text:sub(cursor+1),span.highlight} end
  projection.foldtext[span.start]=chunks
end

function M.refresh_activities(projection, width)
  local changes={}
  projection.running_activity=false
  for _,entry in ipairs(projection.live_activities) do
    local row=entry.span.start
    local title=Tools.activity(entry.record,entry.rows)
    local text='   ▾ '..Tools.fit(title,math.max(0,width-6))
    if entry.record._started_at then projection.running_activity=true end
    if text~=projection.lines[row] then
      changes[#changes+1]={line=row,previous=projection.lines[row],text=text}
      projection.lines[row]=text
      foldtext(projection,entry.span)
    end
  end
  for _,row in ipairs(projection.live_progress or {}) do
    projection.running_activity=true
    local text=projection.lines[row]
    local phase=(vim.uv.hrtime()/1e9)%1
    local first=math.floor((1-math.abs(2*phase-1))*math.max(0,#text-4))
    projection.highlights_by_line[row]={{line=row,group='SosActivity',eol=true},
      {line=row,group='SosValidationSweep',first=first,last=math.min(#text,first+4)}}
    changes[#changes+1]={line=row,previous=text,text=text}
  end
  return changes
end

function M.project(current, info, width)
  local result = {lines={},levels={},spans={},anchors={},line_keys={},responses={},highlights={},highlights_by_line={},links={},plan_lines={},bands={},band_columns={},foldtext={},live_activities={},live_progress={},controls={}}
  result.width=width
  local function highlight(group, first, last, eol)
    if first then
      local length=#result.lines[#result.lines]
      if first>=length then return end
      last=math.min(last or length,length)
    end
    local mark={line=#result.lines,group=group,first=first,last=last,eol=eol}
    table.insert(result.highlights,mark)
    local row=result.highlights_by_line[mark.line] or {}
    result.highlights_by_line[mark.line]=row
    row[#row+1]=mark
  end
  local function line(text, key, level, group)
    table.insert(result.lines,text)
    table.insert(result.levels,level or 0)
    result.anchors[key] = #result.lines
    result.line_keys[#result.lines] = key
    if group then highlight(group,nil,nil,true) end
  end
  local function body(text,key,level,group,markdown,padding)
    local rows=markdown and Markdown.project(text) or vim.tbl_map(function(value) return {text=value} end,vim.split(text or '', '\n',{plain=true}))
    local indent=string.rep(' ',padding or (level or 0)*2)
    for n,row in ipairs(rows) do
      line(indent..row.text,key..'/'..n,level,group)
      if row.group then highlight(row.group,nil,nil,true) end
      for _,mark in ipairs(row.marks or {}) do highlight(mark.group,mark.first+#indent,mark.last+#indent) end
      if row.links and #row.links>0 then
        result.links[#result.lines]=vim.deepcopy(row.links)
        for _,link in ipairs(result.links[#result.lines]) do link.first=link.first+#indent; link.last=link.last+#indent end
      end
    end
  end
  local function separator(key,level,group)
    line(string.rep('─',width or 32),key,level,group or 'SosSeparator')
  end
  local function start(title,key,level,selection,group,offset)
    group=group or 'SosPlan'
    local span = vim.tbl_extend('force',{key=key,start=#result.lines+1,level=level,highlight=group},selection or {})
    offset=offset or 1
    line(string.rep(' ',offset)..'▾ '..Tools.fit(title,width and math.max(0,width-offset-3)),key,'>' .. level,group)
    table.insert(result.spans,span)
    return span
  end
  local function finish(span)
    span.finish = #result.lines
  end
  local function plan(value,key,updated)
    result.responses[key]=M.text(value)
    separator(key..'/separator',0,updated and 'SosSane' or nil)
    local span = start(updated and 'Objective plan · updated' or 'Objective plan',key,1,nil,
      updated and 'SosObjectivePlanUpdated' or 'SosObjectivePlan')
    body(M.text(value),key..'/content',1,'SosPlanBody',true)
    if type(value.objective_stack) == 'string' and value.objective_stack ~= '' then
      line('  Stack',key..'/stack-heading',1,'SosHeading')
      body(value.objective_stack,key..'/stack',1,'SosPlanBody',true)
    end
    finish(span)
    for n=span.start,span.finish do result.plan_lines[n]=true end
    line('',key..'/end',0)
  end
  local versions, turns, last = {}, {}, {}
  for _, version in ipairs((info or {}).objective_plans or {}) do versions[version.id]=version end
  for _, turn in ipairs((info or {}).objective_plan_turns or {}) do turns[turn.message_id]=turn end
  for n, record in ipairs(current.records) do if record.role == 'assistant' then last[record.id or n]=n end end
  local occurrences,seen_activity = {},false
  local history = current.history
  for n, record in ipairs(current.records) do
    local id = record.id or tostring(n)
    occurrences[id]=(occurrences[id] or 0)+1
    local key = 'message:'..id..':'..occurrences[id]
    if record.role == 'activity' then
      local rows = history and history:rows(record) or record.activities
      -- Empty Activities still carry generation timing, but have no lazy payload.
      local has_details=(record.count or 0)>0 or (rows and #rows>0)
      key = 'activity:'..id
      if seen_activity then separator(key..'/separator',0) end
      seen_activity=true
      local title,kind_marks=Tools.activity(record,rows)
      local outer = start(title,key,1,has_details and {group=id} or {},record.label=='Compaction' and 'SosError' or 'SosActivity',3)
      if record._started_at then
        result.running_activity=true
        result.live_activities[#result.live_activities+1]={record=record,rows=rows,span=outer}
      end
      for _,mark in ipairs(kind_marks) do highlight(mark.group,mark.first+3+#'▾ ',mark.last+3+#'▾ ') end
      local cache = history and history.cache[id]
      if not rows and has_details then
        body(cache and cache.errors.group and ('Could not load: '..cache.errors.group..' · Close and reopen to retry')
          or cache and cache.loading.group and 'Loading activities...' or 'Expand to load activities',key..'/status',1,'SosMuted',false,5)
      else
        for _, item in ipairs(rows or {}) do
          local index = item.activityIndex
          local itemkey = key..'/item/'..tostring(index)
          local label,group,title_marks=Tools.title(item,width and math.max(0,width-8))
          if item.toolName == 'read' and not (item.result and item.result.isError) then
            line('       '..Tools.fit(label,width and math.max(0,width-8)),itemkey,1,group) -- Successful read bodies are never fetched from history.
            for _,mark in ipairs(title_marks or {}) do highlight(mark.group,mark.first+7,mark.last+7) end
          else
            local nested = start(label,itemkey,2,{group=id,index=index},group,5)
            for _,mark in ipairs(title_marks or {}) do highlight(mark.group,mark.first+5+#'▾ ',mark.last+5+#'▾ ') end
            local detail = item.live and item or cache and cache.details[index]
            if detail then
              for n,row in ipairs(Tools.details(detail)) do
                local text=row.separator and string.rep('─',math.max(1,(width or 32)-10)) or row.text
                line('         '..text,itemkey..'/detail/'..n,2,row.group)
                for _,mark in ipairs(row.marks or {}) do highlight(mark.group,mark.first+9,mark.last+9) end
              end
            else
              body(cache and cache.errors[index] and ('Could not load: '..cache.errors[index]..' · Close and reopen to retry')
                or cache and cache.loading[index] and 'Loading details...' or 'Expand to load details',itemkey..'/status',2,'SosMuted',false,9)
            end
            finish(nested)
          end
        end
      end
      finish(outer)
    elseif record.role=='construction' then
      if record.progress then
        local progress=record.progress
        local size=math.max(4,math.min(40,(width or 60)-12))
        local filled=progress.total>0 and math.floor(size*progress.done/progress.total) or 0
        if progress.validating then
          line(' Validating',key..'/validating',0,'SosActivity')
          result.live_progress[#result.live_progress+1]=#result.lines
          result.running_activity=true
        end
        line(' '..progress.done..'/'..progress.total..' ['..string.rep('#',filled)..string.rep('-',size-filled)..']',key,0,'SosActivity')
        if progress.validating then result.live_progress[#result.live_progress+1]=#result.lines end
      else
        local first=#result.lines+1
        body(M.text(record),key,0,record.group,true,1)
        if record.control then
          for row=first,#result.lines do if result.lines[row]:match('%S') then result.controls[row]=record.control end end
        end
      end
    elseif record.objective_plan then
      plan(record,'plan:'..id,false)
    elseif record.role ~= 'system' and record.role ~= 'tool' then
      local text = M.text(record)
      if record.role=='user' then seen_activity=false end
      if text ~= '' then
        local user=record.role=='user'
        if user and #result.lines>0 then line('',key..'/spacing',0) end
        local first=#result.lines+1
        body(text,key..'/text',0,nil,not user,user and 3 or 1)
        result.anchors[key]=first
        if user then
          result.user={key=key,line=first,record=record}
          local mode=(record.metadata or {}).mode or current.mode
          for row=first,#result.lines do result.bands[row]=mode=='slop' and 'SosSlop' or 'SosSane' end
        else result.responses[key]=text end
      end
      local turn = turns[record.id]
      if last[record.id] == n and turn and versions[turn.objective_id] then
        plan(versions[turn.objective_id],'turn-plan:'..id,turn.updated)
      end
      if record.role=='assistant' and last[record.id or n]==n then
        local footer=Tools.generation(record.metadata)
        if footer then
          footer=Tools.fit(footer,width and math.max(0,width-2))
          line(string.rep(' ',math.max(1,(width or 32)-1-vim.fn.strdisplaywidth(footer)))..footer,key..'/generation',0,'SosMuted')
        end
      end
    end
  end
  if current.queued and not (history and history.page.hasNewer) then
    result.responses.queue=current.queued
    line('','queue:spacing',0)
    line(' [Pending]','queue:label',0,'SosMuted')
    body(current.queued,'queue:text',0,nil,false,1)
  end
  if history and history.loading then line('Loading history...','history:loading',0) end
  if current.notice then body('Notice: '..current.notice,'notice',0,'SosNotice') end
  if #result.lines == 0 then line('','empty',0) end
  -- Native folds retain the same offsets and semantic spans as expanded headers.
  for _,span in ipairs(result.spans) do
    foldtext(result,span)
  end
  return result
end

return M
