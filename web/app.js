'use strict';
const graph=JSON.parse(document.getElementById('graph-data').textContent);
const nodes=new Map(graph.nodes.map(n=>[n.id,n]));
const outgoing=new Map(),incoming=new Map();
for(const e of graph.edges){if(!outgoing.has(e.from))outgoing.set(e.from,[]);if(!incoming.has(e.to))incoming.set(e.to,[]);outgoing.get(e.from).push(e);incoming.get(e.to).push(e);}
const $=id=>document.getElementById(id), out=id=>outgoing.get(id)||[], inc=id=>incoming.get(id)||[];
const reports=graph.nodes.filter(n=>n.kind==='Report').sort((a,b)=>a.props.number-b.props.number);
const dates=reports.map(r=>r.props.published_date).filter(Boolean).sort();
const defaultFrom=dates[0]||'0001-01-01',defaultTo=dates[dates.length-1]||'9999-12-31';
const operational=new Set(['Passage','ProcessingAttempt','ResolutionAttempt','EvidenceSnapshot','CollectionRun','AcquisitionFailure','Source']);
let visibleReports=new Set(),visible=new Set(),selected=null,focusReport=null,view='report',scale=1,dx=0,dy=0;
const el=(tag,text,cls)=>{const n=document.createElement(tag);if(text!==undefined)n.textContent=text;if(cls)n.className=cls;return n;};
const svg=(tag,attrs)=>{const n=document.createElementNS('http://www.w3.org/2000/svg',tag);for(const [k,v] of Object.entries(attrs))n.setAttribute(k,String(v));return n;};
const link=(label,id)=>{const b=el('button',label,'link');b.addEventListener('click',()=>select(id));return b;};
const reportIDs=n=>{const ids=new Set();if(n.kind==='Report')ids.add(n.id);if(n.props.report)ids.add(n.props.report);for(const p of n.support||[])if(nodes.has(p))ids.add(nodes.get(p).props.report);if(n.kind==='Idea')for(const e of inc(n.id))if(e.kind==='EXPRESSES')for(const r of reportIDs(nodes.get(e.from)))ids.add(r);return ids;};
const isInView=n=>[...reportIDs(n)].some(id=>visibleReports.has(id));
const colour=n=>n.kind==='Organisation'?(out(n.id).some(e=>e.kind==='SUBSIDIARY_OF'&&e.support.some(p=>visibleReports.has(nodes.get(p).props.report)))?'#ecd761':'#f0a04b'):n.kind==='Judgment'?(n.props.direction==='UP'?'#67d396':'#f17777'):({Report:'#84a9ff',Expression:'#8cb4df',Idea:'#b99aff',Stock:'#ffffff',Event:'#61c8d1',Claim:'#78d2c0',Assertion:'#76a7aa',Confirmation:'#64c49d',Domain:'#b4bd75',OrganisationActivity:'#a6b66d',Resource:'#dc9cbf',Process:'#d8a9db',Person:'#b2b8cb'})[n.kind]||'#788b9f';
const safeLabel=n=>n.label.length>70?n.label.slice(0,67)+'…':n.label;
function updateFilters(){
 const search=$('search').value.toLowerCase(),stock=$('stock').value,host=$('source').value;
 let orgs=new Set();if(stock){const s=graph.nodes.find(n=>n.kind==='Stock'&&n.props.symbol===stock);for(const e of out(s.id))if(e.kind==='CONFIGURED_ISSUER')orgs.add(e.to);}
 visibleReports=new Set(reports.filter(r=>{
  const date=$('time').value==='strict'?r.props.source_available_at:r.props.published_date;
  if(!date||date.slice(0,10)<$('from').value||date.slice(0,10)>$('to').value||(host&&r.props.source_host!==host))return false;
  const expressions=out(r.id).filter(e=>e.kind==='CONTAINS').map(e=>nodes.get(e.to));
  if(stock&&!out(r.id).some(e=>e.kind==='MENTIONS'&&orgs.has(e.to))&&!expressions.some(n=>out(n.id).some(e=>e.kind==='CONCERNS'&&orgs.has(e.to))))return false;
  return !search||[r.label,...expressions.map(n=>n.label)].join(' ').toLowerCase().includes(search);
 }).map(r=>r.id));
 if(!visibleReports.has(focusReport))focusReport=[...visibleReports][0]||null;
 $('report-count').textContent=`${visibleReports.size} / ${reports.length}`;
 $('reports').replaceChildren();
 for(const r of reports)if(visibleReports.has(r.id)){const b=el('button',`#${r.props.number} · ${r.label}`,'report'+(r.id===focusReport?' selected':''));b.append(el('small',`${r.props.published_date} · ${r.props.source_host}`));b.append(el('small',`${r.props.processing_status||'UNPROCESSED'} · ${r.props.inspection.status}`));b.addEventListener('click',()=>{focusReport=r.id;view='report';updateFilters();select(r.id);});$('reports').append(b);}
  const unknown=reports.filter(r=>!r.props.source_available_at).length;
  $('coverage').textContent=$('time').value==='strict'?`Strict availability: ${unknown} of ${reports.length} reports lack supported exact-version first-availability dates. Empty results do not mean no news occurred.`:'Exploratory review uses declared publication dates and retained publisher text. Collection limits and processing gaps are available in the coverage details. First-party pages confirm only scoped announcements; plans remain plans.';
 if(view==='report')buildReport(focusReport);else if(view==='world')buildWorld();else buildOverview();
 const picker=$('node-picker');picker.replaceChildren(el('option','Choose a node…'));for(const id of [...visible].sort((a,b)=>nodes.get(a).kind.localeCompare(nodes.get(b).kind)||nodes.get(a).label.localeCompare(nodes.get(b).label))){const o=el('option',`${nodes.get(id).kind}: ${safeLabel(nodes.get(id))}`);o.value=id;picker.append(o);}
 if(!selected||(!visible.has(selected)&&!operational.has(nodes.get(selected)?.kind)))selected=focusReport;
 draw();showDetail();
}
function buildReport(id){
 visible=new Set();if(!id)return;visible.add(id);
 const primary=new Set(['Expression','Idea','Judgment','Organisation']);
 for(const n of graph.nodes)if(primary.has(n.kind)&&(n.props.report===id||(n.support||[]).some(p=>nodes.get(p).props.report===id)))visible.add(n.id);
 for(const e of out(id))if(['CONTAINS','MENTIONS'].includes(e.kind)&&primary.has(nodes.get(e.to).kind))visible.add(e.to);
 expandStructural();
}
function expandStructural(){
 for(let i=0;i<2;i++)for(const id of [...visible]){
  for(const e of out(id))if((view==='report'?['CONCERNS','EXPRESSES','TARGETS','HAS_JUDGMENT']:['CONCERNS','CONCERNS_ACTIVITY','EXPRESSES','TARGETS','ABOUT_CLAIM','ABOUT_EVENT','IN_DOMAIN','HAS_JUDGMENT','ATTRIBUTED_TO']).includes(e.kind))visible.add(e.to);
  for(const e of inc(id))if(e.kind==='CONFIGURED_ISSUER')visible.add(e.from);
 }
}
function buildOverview(){visible=new Set(graph.nodes.filter(n=>['Report','Expression','Idea','Organisation','Stock','Judgment'].includes(n.kind)&&isInView(n)).map(n=>n.id));expandStructural();}
function buildWorld(){visible=new Set(graph.nodes.filter(n=>['Organisation','Stock','Event','Claim','Resource','Process'].includes(n.kind)&&isInView(n)).map(n=>n.id));expandStructural();}
function draw(){
 const scene=$('scene');scene.replaceChildren();const ids=[...visible];const cap=220;const shown=ids.slice(0,cap);const display=new Set(shown);
 const columns=new Map();const column=n=>(view==='report'?{Report:0,Expression:1,Idea:2,Judgment:2,Organisation:3,Stock:4}:{Report:0,Expression:1,Idea:2,Judgment:2,Event:2,Assertion:1,Claim:3,Confirmation:2,Organisation:4,OrganisationActivity:5,Domain:6,Stock:5,Resource:5,Process:5,Person:0})[n.kind]??3;
 for(const id of shown){const col=column(nodes.get(id));if(!columns.has(col))columns.set(col,[]);columns.get(col).push(id);}
 const pos=new Map();let maxHeight=500;for(const [col,list] of columns){maxHeight=Math.max(maxHeight,list.length*110+100);list.forEach((id,i)=>pos.set(id,{x:80+col*245,y:110+i*110}));}
 const width=Math.max(700,Math.max(0,...columns.keys())*245+230);$('graph').setAttribute('viewBox',`-45 0 ${width} ${maxHeight}`);
 for(const e of graph.edges){if(!display.has(e.from)||!display.has(e.to))continue;if(e.support.length&&!e.support.every(p=>visibleReports.has(nodes.get(p).props.report)))continue;
  const a=pos.get(e.from),b=pos.get(e.to),path=svg('path',{d:`M${a.x},${a.y} C${(a.x+b.x)/2},${a.y} ${(a.x+b.x)/2},${b.y} ${b.x},${b.y}`,class:'edge','marker-end':'url(#arrow)'});const title=svg('title',{});title.textContent=e.kind+(e.props.description?': '+e.props.description:'');path.append(title);scene.append(path);
  if(shown.length<65){const label=svg('text',{x:(a.x+b.x)/2,y:(a.y+b.y)/2-5,class:'edge-label','text-anchor':'middle'});label.textContent=e.kind;scene.append(label);}}
 for(const id of shown){const n=nodes.get(id),p=pos.get(id),g=svg('g',{transform:`translate(${p.x} ${p.y})`,class:'node'+(id===selected?' selected':''),'data-id':id,tabindex:0,role:'button','aria-label':n.kind+': '+n.label});
  g.append(svg('circle',{r:n.kind==='Idea'?16:13,fill:colour(n)}));const title=svg('title',{});title.textContent=n.kind+': '+n.label;g.append(title);
  const text=svg('text',{x:0,y:34,'text-anchor':'middle',style:'font-size:19px'});const label=n.kind==='Report'?`#${n.props.number} ${n.label}`:n.label;for(let line=0;line<2;line++){const span=svg('tspan',{x:0,dy:line?22:0});span.textContent=label.slice(line*23,(line+1)*23)+(line===1&&label.length>46?'…':'');text.append(span);}g.append(text);
  g.addEventListener('click',()=>select(id));g.addEventListener('keydown',event=>{if(event.key==='Enter')select(id);});scene.append(g);}
 $('graph-status').textContent=shown.length?`${view==='report'?'Selected article':view==='world'?'Connected world':'All meanings'} · ${shown.length} nodes${ids.length>cap?` shown of ${ids.length}; use article focus for detail`:''} · click a node to inspect`:'No eligible records for these filters';transform();
}
function select(id){if(!nodes.has(id))return;selected=id;$('node-picker').value=id;for(const n of document.querySelectorAll('#scene .node'))n.classList.toggle('selected',n.dataset.id===id);showDetail();}
function showDetail(){
 const panel=$('detail');panel.replaceChildren();if(!selected||!nodes.has(selected)||!visibleReports.size){panel.append(el('p','Select a report or semantic node to inspect its meaning, exact evidence and processing output.','empty'));return;}
 const n=nodes.get(selected);panel.append(el('span',n.kind,'pill'),el(n.label.length>110?'p':'h2',n.label));panel.lastChild.id='selection-title';
 if(n.kind==='Judgment'){panel.append(el('h3',`${n.props.stock} · ${n.props.direction}`,n.props.direction==='UP'?'up':'down'));panel.append(el('p',n.props.summary));panel.append(el('p',n.props.conditions?`Condition: ${n.props.conditions}`:'Qualification: unqualified in retained extraction'));panel.append(el('p',`Horizon: ${n.props.horizon?.wording||'unstated'} · ${n.props.window?.origin}`));panel.append(el('p',n.props.window?.start?`${n.props.window.start} to ${n.props.window.end} (exclusive)`:'No numeric window inferred'));}
 if(n.kind==='Report'){panel.append(el('p',`${n.props.published_at} · ${n.props.source_host}`));const a=el('a','Open original publisher page');if(/^https?:\/\//i.test(n.props.url||''))a.href=n.props.url;a.target='_blank';a.rel='noopener noreferrer';panel.append(a);panel.append(el('p',`Text: ${n.props.inspection.status}`));panel.append(el('p',`Processing: ${n.props.processing_status||'not prepared'}`));panel.append(el('small',`Collected ${n.props.collected_at}; exact-version first availability unknown.`));if(n.props.issues?.length)panel.append(el('p',JSON.stringify(n.props.issues),'notice'));if(n.props.validation_failures?.length)panel.append(el('p',JSON.stringify(n.props.validation_failures),'notice'));
  panel.append(el('h3','Extracted expressions'));for(const e of out(n.id))if(e.kind==='CONTAINS')panel.append(link(nodes.get(e.to).label,e.to));}
 if(n.kind==='Expression'){panel.append(el('p',`Kind: ${n.props.kind} · expressed agreement: ${n.props.agreement||'unknown / not expressed'}`));const judgments=out(n.id).filter(e=>e.kind==='HAS_JUDGMENT');if(!judgments.length)panel.append(el('p','No supported absolute stock-price UP/DOWN judgment.','notice'));for(const e of judgments)panel.append(link(nodes.get(e.to).label,e.to));}
 if(n.props.status)panel.append(el('p',`Status: ${n.props.status}`,'notice'));if(n.props.confirmation_scope)panel.append(el('p',n.props.confirmation_scope,'notice'));if(n.props.modality)panel.append(el('p',`Source modality: ${n.props.modality}`));
 if(n.kind==='Idea'){const expressions=inc(n.id).filter(e=>e.kind==='EXPRESSES'&&isInView(nodes.get(e.from)));panel.append(el('p',`${expressions.length} source expressions · ${new Set(expressions.map(e=>nodes.get(e.from).props.report)).size} distinct publications in this view`));for(const e of expressions)panel.append(link(nodes.get(nodes.get(e.from).props.report).label,e.from));}
 const support=n.kind==='Passage'?[n.id]:n.support||[];if(support.length){panel.append(el('h3','Exact supporting passages'));for(const id of support){const p=nodes.get(id);if(!visibleReports.has(p.props.report))continue;const r=nodes.get(p.props.report);panel.append(link(`#${r.props.number} · ${r.label}`,r.id));panel.append(el('blockquote',p.props.quote));panel.append(el('small',`UTF-8 bytes [${p.props.begin}, ${p.props.end}) · body ${p.props.body_sha256.slice(0,16)}…`));}}
 const edges=[...out(n.id).map(e=>[e,e.to,'→']),...inc(n.id).map(e=>[e,e.from,'←'])];panel.append(el('h3','Relationships'));for(const [e,id,arrow] of edges){const target=nodes.get(id);if(e.support.length&&!e.support.every(p=>visibleReports.has(nodes.get(p).props.report)))continue;panel.append(link(`${arrow} ${e.kind} · ${safeLabel(target)}`,id));if(e.props.description)panel.append(el('small',e.props.description));}
 const related=new Set(reportIDs(n));for(const r of related)if(visibleReports.has(r)){
  const source=nodes.get(r);const body=el('details');body.append(el('summary',`Retained source text · #${source.props.number}`),el('div',source.props.body_text,'source-body'));panel.append(body);
  for(const e of inc(r))if(e.kind==='PROCESSED'){const attempt=nodes.get(e.from);const d=el('details');d.append(el('summary','Original model output & request provenance'),el('pre',JSON.stringify(attempt.props,null,2)));panel.append(d);}}
 const raw=el('details');raw.append(el('summary','Native record / IDs'),el('pre',JSON.stringify(n,null,2)));panel.append(raw);
}
function transform(){$('scene').setAttribute('transform',`translate(${dx} ${dy}) scale(${scale})`);}
for(const id of ['stock','source','from','to','time'])$(id).addEventListener('change',updateFilters);$('search').addEventListener('input',updateFilters);$('node-picker').addEventListener('change',e=>select(e.target.value));
$('overview').onclick=()=>{view='overview';updateFilters();};$('world').onclick=()=>{view='world';updateFilters();};
$('reset').onclick=()=>{$('stock').value='';$('source').value='';$('search').value='';$('from').value=defaultFrom;$('to').value=defaultTo;$('time').value='declared_date';view='report';focusReport=(reports.find(r=>r.props.number===17)||reports[0])?.id;selected=focusReport;scale=1;dx=dy=0;updateFilters();};
$('zoom-in').onclick=()=>{scale=Math.min(5,scale*1.25);transform();};$('zoom-out').onclick=()=>{scale=Math.max(.2,scale/1.25);transform();};$('fit').onclick=()=>{scale=1;dx=dy=0;transform();};
$('expand').onclick=()=>{if(!selected)return;visible=new Set([selected]);for(const e of [...out(selected),...inc(selected)])if(!operational.has(nodes.get(e.from).kind)&&!operational.has(nodes.get(e.to).kind)&&(!e.support.length||e.support.every(p=>visibleReports.has(nodes.get(p).props.report)))){visible.add(e.from);visible.add(e.to);}expandStructural();view='neighbourhood';draw();};
let drag=null;$('graph').addEventListener('pointerdown',e=>{if(e.target.closest('.node'))return;drag={x:e.clientX,y:e.clientY,dx,dy};$('graph').setPointerCapture(e.pointerId);});$('graph').addEventListener('pointermove',e=>{if(!drag)return;const box=$('graph').getBoundingClientRect(),ratio=$('graph').viewBox.baseVal.width/box.width;dx=drag.dx+(e.clientX-drag.x)*ratio;dy=drag.dy+(e.clientY-drag.y)*ratio;transform();});$('graph').addEventListener('pointerup',()=>{drag=null;});
for(const host of [...new Set(reports.map(r=>r.props.source_host))].sort()){const option=el('option',host);option.value=host;$('source').append(option);}
$('metrics').textContent=`${reports.length} reports · ${graph.nodes.filter(n=>n.kind==='Expression').length} expressions · ${graph.nodes.filter(n=>n.kind==='Idea').length} ideas · ${graph.nodes.filter(n=>n.kind==='Organisation').length} organisations`;
$('provenance').textContent=`${reports.length} retained publications · ${graph.meta.source_change||'Labelled fixture'} · present-day interpretations; source assertions are not independently verified outcomes.`;
const coverageDetail=el('details');coverageDetail.append(el('summary','Collection and processing coverage'),el('pre',JSON.stringify(graph.meta.coverage,null,2)));$('coverage').after(coverageDetail);
window.axthenaReview={graph,nodes,select,get visible(){return [...visible];},get selected(){return selected;}};
$('reset').click();
