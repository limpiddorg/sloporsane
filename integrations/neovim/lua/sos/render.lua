-- Pure transcript projection: no network requests, file reads or tool replay.
local M = {}
function M.text(record)
  if type(record.content) == 'string' then return record.content end
  local parts = {}
  for _, block in ipairs(record.content or {}) do
    if block.type == 'text' and type(block.text) == 'string' then table.insert(parts,block.text) end
  end
  return table.concat(parts,'\n')
end

function M.fold(lnum)
  return (vim.b.sos_foldlevels or {})[lnum] or 0
end

function M.foldtext()
  return '▸ '..vim.fn.getline(vim.v.foldstart)..'  · '..(vim.v.foldend-vim.v.foldstart)..' lines'
end

function M.project(current, info)
  local result = {lines={},levels={},spans={},anchors={},line_keys={},highlights={}}
  local function highlight(group, first, last, eol)
    table.insert(result.highlights,{line=#result.lines,group=group,first=first,last=last,eol=eol})
  end
  local function line(text, key, level, group)
    table.insert(result.lines,text)
    table.insert(result.levels,level or 0)
    result.anchors[key] = #result.lines
    result.line_keys[#result.lines] = key
    if group then highlight(group,nil,nil,true) end
  end
  local function body(text,key,level,group,markdown)
    local fenced=false
    for n, value in ipairs(vim.split(text or '', '\n',{plain=true})) do
      local code=value:match('^%s*```') or value:match('^%s*~~~')
      line(value,key..'/'..n,level,group)
      if markdown then
        if code then fenced=not fenced end
        if code or fenced then highlight('SosCode')
        elseif value:match('^%s*#+%s') then highlight('SosHeading')
        else
          for first, content, last in value:gmatch('()`([^`]+)`()') do
            highlight('SosCode',first-1,last-1)
          end
          for first, content, last in value:gmatch('()%*%*([^*]+)%*%*()') do
            highlight('SosBold',first-1,last-1)
          end
        end
      end
    end
  end
  local function start(title,key,level,selection)
    local span = vim.tbl_extend('force',{key=key,start=#result.lines+1,level=level},selection or {})
    line(title,key,'>' .. level)
    highlight(level==2 and 'SosDetail' or (selection and 'SosActivity' or 'SosPlan'),nil,nil,true)
    table.insert(result.spans,span)
    return span
  end
  local function finish(span)
    span.finish = #result.lines
  end
  local function plan(value,key,updated)
    local span = start(updated and 'Objective plan · updated' or 'Objective plan',key,1)
    body(M.text(value),key..'/content',1,nil,true)
    if type(value.objective_stack) == 'string' and value.objective_stack ~= '' then
      body(value.objective_stack,key..'/stack',1)
    end
    finish(span)
    line('',key..'/end',0)
  end
  local versions, turns, last = {}, {}, {}
  for _, version in ipairs((info or {}).objective_plans or {}) do versions[version.id]=version end
  for _, turn in ipairs((info or {}).objective_plan_turns or {}) do turns[turn.message_id]=turn end
  for n, record in ipairs(current.records) do if record.role == 'assistant' then last[record.id or n]=n end end
  local occurrences = {}
  local history = current.history
  for n, record in ipairs(current.records) do
    local id = record.id or tostring(n)
    occurrences[id]=(occurrences[id] or 0)+1
    local key = 'message:'..id..':'..occurrences[id]
    if record.role == 'activity' then
      local rows = history and history:rows(record) or record.activities
      -- A user message creates a live placeholder before tools/reasoning exist.
      -- It is not a real Activity and must not offer a bogus lazy-detail drawer.
      if (record.count or 0)>0 or (rows and #rows>0) then
      key = 'activity:'..id
      local outer = start('┌ Activity · '..tostring(rows and #rows or record.count)..' ────────┐',key,1,{group=id})
      local cache = history and history.cache[id]
      if not rows then
        body(cache and cache.errors.group and ('Could not load: '..cache.errors.group..' · Close and reopen to retry')
          or cache and cache.loading.group and 'Loading activities...' or 'Expand to load activities',key..'/status',1)
      else
        for _, item in ipairs(rows) do
          local index = item.activityIndex
          local itemkey = key..'/item/'..tostring(index)
          local args = item.arguments or {}
          local label = item.kind == 'reasoning' and 'Reasoning' or tostring(item.toolName or 'Tool')
          local hint = args.mainCommand or args.pattern or args.filePath
          if hint then label = label..' · '..tostring(hint):gsub('[\r\n]',' ') end
          if item.result then label=label..(item.result.isError and ' · Failed' or ' · Completed') end
          if item.toolName == 'read' then
            line('  '..label,itemkey,1) -- Successful read bodies are never fetched from history.
          else
            local nested = start('  '..label,itemkey,2,{group=id,index=index})
            local detail = item.live and item or cache and cache.details[index]
            if detail then
              if detail.kind == 'reasoning' then body(detail.text or detail.content or '',itemkey..'/detail',2)
              else
                body(vim.inspect(detail.arguments or {}),itemkey..'/args',2)
                if detail.result then
                  local output = detail.result.output
                  body(type(output)=='string' and output or vim.inspect(output),itemkey..'/result',2)
                end
              end
            else
              body(cache and cache.errors[index] and ('Could not load: '..cache.errors[index]..' · Close and reopen to retry')
                or cache and cache.loading[index] and 'Loading details...' or 'Expand to load details',itemkey..'/status',2)
            end
            finish(nested)
          end
        end
        if #rows == 0 then line('No activity details',key..'/empty',1) end
      end
      line('└────────────────────────────┘',key..'/border',1,'SosActivity')
      finish(outer)
      line('',key..'/end',0)
      end
    elseif record.objective_plan then
      plan(record,'plan:'..id,false)
    elseif record.role ~= 'system' and record.role ~= 'tool' then
      local text = M.text(record)
      if text ~= '' then
        if record.role=='user' then line('────────────────────────────────',key..'/separator',0,'SosSeparator') end
        line(record.role=='user' and 'You' or record.role=='assistant' and 'SOS' or (record.role or 'message'),key,0,
          record.role=='user' and 'SosUser' or 'SosAssistant')
        body(text,key..'/text',0,nil,true)
        line('',key..'/end',0)
      end
      local turn = turns[record.id]
      if last[record.id] == n and turn and versions[turn.objective_id] then
        plan(versions[turn.objective_id],'turn-plan:'..id,turn.updated)
      end
    end
  end
  if history and history.loading then line('Loading history...','history:loading',0) end
  if current.notice then body('Notice: '..current.notice,'notice',0,'SosNotice') end
  if #result.lines == 0 then line('SOS conversation','empty',0) end
  return result
end

return M
