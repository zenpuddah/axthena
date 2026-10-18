export type RecordNode={id:string;kind:string;label:string;props:Record<string,any>;support:string[];evidence?:RecordNode[]};
export type Edge={id:string;kind:string;from:string;to:string;props:Record<string,any>;support:string[]};
export type Plan={intent:string;stocks:string[];sources:string[];from:string;to:string;mode:string;max_hops:number;limit:number;text?:string;domain?:string;id?:string};
export type QueryResult={run_id:string;request:any;nodes:RecordNode[];edges:Edge[];distinct_reports:number;truncated:boolean;grouping_exclusions:any[];holds:any[];coverage:{available_reports:number;eligible_reports:number;selected_reports:number;exclusions:any[];processing:any[]}};
export type Answer={run_id:string;plan:Plan;results:QueryResult[];findings?:{text:string;citations:string[]}[];summary_error?:string;summary_scope?:string;planner?:any};
export const initialPlan:Plan={intent:'overview',stocks:['NVDA.US'],sources:[],from:'2025-01-01',to:'2025-12-31',mode:'declared_date',max_hops:0,limit:5000};
export const colors:Record<string,string>={Organisation:'#ffb06a',Domain:'#8bd5c5',Idea:'#a9a0ff',Expression:'#a9a0ff',Event:'#75baff',Claim:'#f1d189',Judgment:'#d9e4f4',Report:'#94a7be',Stock:'#ffb06a'};
export function records(answer:Answer|null){const nodes=new Map<string,RecordNode>(),edges=new Map<string,Edge>();for(const result of answer?.results??[]){for(const n of result.nodes)nodes.set(n.id,n);for(const e of result.edges)edges.set(e.id,e);}return {nodes,edges:[...edges.values()]};}
export function activityCounts(answer:Answer|null){
 const {nodes,edges}=records(answer);const groups=new Map<string,{id:string;label:string;reports:Set<string>;subjects:Set<string>}>();
 const stocks=new Set([...nodes.values()].filter(n=>n.kind==='Stock'&&answer?.plan.stocks.includes(n.props.symbol)).map(n=>n.id));
 const organisations=new Set(edges.filter(e=>e.kind==='CONFIGURED_ISSUER'&&stocks.has(e.from)).map(e=>e.to));
 for(const e of edges){if(e.kind!=='CONCERNS_ACTIVITY')continue;const subject=nodes.get(e.from),activity=nodes.get(e.to);if(!subject||!activity||!subject.props.report)continue;
 if(answer?.plan.stocks.length&&!organisations.has(activity.props.organisation))continue;
 const domain=nodes.get(activity.props.domain);if(!domain)continue;
 let g=groups.get(domain.id);if(!g){g={id:domain.id,label:domain.label,reports:new Set(),subjects:new Set()};groups.set(domain.id,g);}g.reports.add(subject.props.report);g.subjects.add(subject.id);
 }return [...groups.values()].map(g=>({id:g.id,label:g.label,reports:g.reports.size,subjects:g.subjects.size})).sort((a,b)=>b.reports-a.reports||a.label.localeCompare(b.label));
}
export function publicationSeries(answer:Answer|null){
 const {nodes}=records(answer);const months=new Map<string,Set<string>>();for(const n of nodes.values())if(n.kind==='Report'){
 const date=String(n.props.published_date??'');if(date< String(answer?.plan.from)||date>String(answer?.plan.to))continue;
 const key=date.slice(0,7);if(key.length!==7)continue;const values=months.get(key)??new Set();values.add(n.id);months.set(key,values);
 }return [...months].sort(([a],[b])=>a.localeCompare(b)).map(([month,ids])=>({month,reports:ids.size}));
}
export function graphElements(answer:Answer|null,focus:string|null,all=false,limit=all?500:36){
 const {nodes,edges}=records(answer);const kinds=new Set(['Organisation','Domain','Idea','Event','Claim','Judgment']);
 const eligible=[...nodes.values()].filter(n=>kinds.has(n.kind));
 const meanings=new Map(edges.filter(e=>e.kind==='EXPRESSES').map(e=>[e.from,e.to]));
 const projected=edges.flatMap(e=>{
 const from=meanings.get(e.from)??e.from;
 if(e.kind==='CONCERNS_ACTIVITY'){const domain=nodes.get(e.to)?.props.domain;return nodes.has(domain)?[{...e,from,to:domain,kind:'activity (source path)'}]:[];}
 return [{...e,from}];
 }).filter(e=>e.from!==e.to&&nodes.has(e.from)&&nodes.has(e.to)&&kinds.has(nodes.get(e.from)!.kind)&&kinds.has(nodes.get(e.to)!.kind));
 let candidates=eligible;
 if(focus&&nodes.has(focus)){const near=new Set([focus]);for(const e of projected)if(e.from===focus||e.to===focus){near.add(e.from);near.add(e.to);}candidates=eligible.filter(n=>near.has(n.id));}
 const priority=(n:RecordNode)=>n.kind==='Organisation'?0:n.kind==='Idea'?1:n.kind==='Domain'?2:3;
 candidates.sort((a,b)=>priority(a)-priority(b)||a.label.localeCompare(b.label));
 const stockIds=new Set([...nodes.values()].filter(n=>n.kind==='Stock'&&answer?.plan.stocks.includes(n.props.symbol)).map(n=>n.id));
 const anchors=new Set(edges.filter(e=>e.kind==='CONFIGURED_ISSUER'&&stockIds.has(e.from)).map(e=>e.to));
 const selected:RecordNode[]=candidates.filter(n=>anchors.has(n.id));
 const seed=focus?candidates.find(n=>n.id===focus):selected[0]??candidates[0];
 if(focus)selected.length=0;
 if(seed&&!selected.some(n=>n.id===seed.id))selected.push(seed);
 const near=new Map<string,Set<string>>();for(const e of projected){for(const [a,b] of [[e.from,e.to],[e.to,e.from]]){const set=near.get(a)??new Set();set.add(b);near.set(a,set);}}
 const add=(node:RecordNode)=>{if(selected.length<limit&&!selected.some(n=>n.id===node.id))selected.push(node);};
 // Keep a connected, bounded neighborhood rather than sampling unrelated labels.
 if(seed){const direct=candidates.filter(n=>near.get(seed.id)?.has(n.id));direct.slice(0,10).forEach(add);
  for(const subject of [...selected])candidates.filter(n=>near.get(subject.id)?.has(n.id)&&n.kind==='Domain').slice(0,2).forEach(add);
  for(const subject of [...selected])candidates.filter(n=>near.get(subject.id)?.has(n.id)).forEach(add);
 }
 if(selected.length<3)candidates.slice(0,12).forEach(add);
 if(all){selected.length=0;selected.push(...eligible.slice(0,Math.min(limit,500)));}
 const ids=new Set(selected.map(n=>n.id));
 return {total:eligible.length,shown:selected.length,elements:[...selected.map(n=>({data:{id:n.id,label:n.label.length>30?n.label.slice(0,27)+'…':n.label,fullLabel:n.label,kind:n.kind,anchor:n.id===seed?.id,color:colors[n.kind]??'#94a7be',size:anchors.has(n.id)?52:n.kind==='Organisation'?32:n.kind==='Idea'?24:18}})),...projected.filter(e=>ids.has(e.from)&&ids.has(e.to)).map(e=>({data:{id:e.id,source:e.from,target:e.to,label:e.kind}}))]};
}
export function selectedEvidence(answer:Answer|null,id:string|null){
 const {nodes,edges}=records(answer);const node=id?nodes.get(id):undefined;const passages=new Map<string,RecordNode>();
 const add=(n:RecordNode|undefined)=>{if(!n)return;for(const p of n.evidence??[])passages.set(p.id,p);for(const pid of n.support??[]){const p=nodes.get(pid);if(p?.kind==='Passage')passages.set(pid,p);}};
 add(node);if(node?.kind==='Domain')for(const e of edges)if(e.kind==='CONCERNS_ACTIVITY'&&nodes.get(e.to)?.props.domain===id)add(nodes.get(e.from));
 if(node?.kind==='Organisation')for(const e of edges)if(e.kind==='CONCERNS'&&e.to===id)add(nodes.get(e.from));
 return {node,passages:[...passages.values()],nodes};
}
