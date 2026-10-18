import {test} from 'node:test';
import assert from 'node:assert/strict';
import cytoscape from 'cytoscape';
import {displayedPath} from './live-graph';
import {activityCounts,graphElements,publicationSeries,selectedEvidence,initialPlan,type Answer,type RecordNode} from './data';
const node=(id:string,kind:string,props:any={}):RecordNode=>({id,kind,label:id,props,support:[]});
const nodes=[node('r','Report',{published_date:'2025-04-01'}),node('s','Stock',{symbol:'NVDA.US'}),node('o','Organisation'),node('other','Organisation'),node('d','Domain'),node('otherd','Domain'),node('a','OrganisationActivity',{domain:'d',organisation:'o'}),node('b','OrganisationActivity',{domain:'otherd',organisation:'other'}),node('e','Expression',{report:'r'}),node('e2','Expression',{report:'r'}),node('idea','Idea'),node('p','Passage',{quote:'Exact passage',report:'r'})];
nodes.find(n=>n.id==='e')!.support=['p'];
const edges=[['s','o','CONFIGURED_ISSUER'],['e','a','CONCERNS_ACTIVITY'],['e2','a','CONCERNS_ACTIVITY'],['e','b','CONCERNS_ACTIVITY'],['e','idea','EXPRESSES']].map(([from,to,kind],i)=>({id:String(i),from,to,kind,props:{},support:[]}));
const answer:Answer={run_id:'fixture',plan:initialPlan,results:[{nodes,edges,run_id:'fixture',request:initialPlan,distinct_reports:1,truncated:false,grouping_exclusions:[],holds:[],coverage:{available_reports:1,eligible_reports:1,selected_reports:1,exclusions:[],processing:[]}}]};
test('Activity counts deduplicate reports and restrict company-specific activity pairs',()=>assert.deepEqual(activityCounts(answer),[{id:'d',label:'d',reports:1,subjects:2}]));
test('Comparison union counts one publication once',()=>assert.deepEqual(publicationSeries({...answer,results:[...answer.results,...answer.results]}),[{month:'2025-04',reports:1}]));
test('Graph projects expression activity through its actual meaning membership',()=>{const graph=graphElements(answer,null);assert.ok(graph.elements.some((e:any)=>e.data.source==='idea'&&e.data.target==='d'));});
test('Domain evidence drills through activity to exact supported passage',()=>assert.equal(selectedEvidence(answer,'d').passages[0].props.quote,'Exact passage'));
test('Path exploration finds the shortest displayed connection through cycles and respects disconnected scope',()=>{
 const cy=cytoscape({headless:true,elements:[...['a','b','c','d','outside'].map(id=>({data:{id}})),...[
  ['ab','a','b'],['bc','b','c'],['cd','c','d'],['da','d','a'],
 ].map(([id,source,target])=>({data:{id,source,target}}))]});
 assert.deepEqual(displayedPath(cy,'a','d'),['a','da','d']);
 assert.equal(displayedPath(cy,'a','c')?.length,5);
 assert.equal(displayedPath(cy,'a','outside'),null);
 cy.getElementById('ab').remove();cy.getElementById('da').remove();
 assert.equal(displayedPath(cy,'a','c'),null);cy.destroy();
});
