import {chromium,expect} from '@playwright/test';
import assert from 'node:assert/strict';
import {mkdir} from 'node:fs/promises';

const browser=await chromium.launch({channel:'chromium',headless:true});
const base=process.env.AXTHENA_URL??'http://127.0.0.1:17781';
const page=await browser.newPage({viewport:{width:1512,height:1000}});
const errors=[];page.on('pageerror',e=>errors.push(String(e)));
await page.goto(base);
await page.getByRole('button',{name:'Fullscreen graph',exact:true}).waitFor();
// Read-only Cytoscape registration probes check actual rendered styles/positions,
// not just that a range input's displayed number changed.
const probe=()=>page.evaluate(()=>{
 const cy=document.querySelector('.network')._cyreg.cy;
 return {ids:cy.nodes().map(n=>n.id()),positions:cy.nodes().map(n=>n.position()),width:cy.nodes()[0].width(),label:cy.nodes()[0].style('label'),font:`${Math.round(parseFloat(cy.nodes()[0].style('font-size'))*cy.zoom())}px`,opacity:cy.edges()[0]?.style('opacity')};
});
await page.waitForFunction(()=>document.querySelector('.network')?._cyreg?.cy?.nodes().length>0);
const original=await probe();assert.ok(original.ids.length<=36);
await page.getByRole('button',{name:'Fullscreen graph',exact:true}).click();
await expect(page.getByRole('dialog',{name:'Graph explorer'})).toBeVisible();
assert.equal(await page.evaluate(()=>!!document.fullscreenElement),true);
const box=await page.getByRole('dialog',{name:'Graph explorer'}).boundingBox();
assert.equal(box.width,1512);assert.equal(box.height,1000);

await page.keyboard.press('/');
await expect(page.getByRole('searchbox',{name:'Search graph',exact:true})).toBeFocused();
await page.getByRole('searchbox',{name:'Search graph',exact:true}).fill('no-such-entity-xyz');
await expect(page.getByText('No matching entities in this query result.')).toBeVisible();
await page.getByRole('button',{name:'Close graph search'}).click();
await expect(page.getByRole('searchbox',{name:'Search graph',exact:true})).toHaveCount(0);

await page.getByRole('combobox',{name:'Graph display scope'}).selectOption('all');
await page.waitForFunction(()=>document.querySelector('.network')._cyreg.cy.nodes().length>36);
const all=await probe();assert.ok(all.ids.length>original.ids.length);
const hiddenId=all.ids.find(id=>!original.ids.includes(id));
const response=await page.request.post(base+'/api/investigate',{data:{plan:{intent:'overview',stocks:['NVDA.US'],mode:'declared_date',from:'2025-01-01',to:'2025-12-31',max_hops:0,limit:5000}}});
const answer=await response.json();const hidden=answer.results[0].nodes.find(n=>n.id===hiddenId);
await page.getByRole('combobox',{name:'Graph display scope'}).selectOption('neighborhood');
await page.getByRole('button',{name:/^Search/}).click();
await page.getByRole('searchbox',{name:'Search graph',exact:true}).fill(hidden.label);
await page.locator('.graph-search-results button').filter({hasText:hidden.label}).first().click();
await page.waitForFunction(id=>document.querySelector('.network')._cyreg.cy.getElementById(id).length===1,hiddenId);
await expect(page.getByRole('searchbox',{name:'Search graph',exact:true})).toHaveCount(0);
await page.getByRole('button',{name:'Reset focus',exact:true}).click();

let previous;
for(const name of ['circle','grid','concentric','breadthfirst','cose']){
 await page.getByRole('combobox',{name:'Graph layout'}).selectOption(name);
 await expect(page.locator('.network-footer small')).toContainText(({circle:'Circle',grid:'Grid',concentric:'Radial',breadthfirst:'Hierarchical',cose:'Force-directed'})[name]);
 await page.waitForTimeout(400); // Bounded layout debounce/render settling, not process polling.
 const state=await probe();assert.equal(state.ids.length,original.ids.length);
 assert.ok(state.positions.every(p=>Number.isFinite(p.x)&&Number.isFinite(p.y)));
 if(previous)assert.notDeepEqual(state.positions,previous);
 previous=state.positions;
}
await page.getByRole('button',{name:'View settings',exact:true}).click();
async function slide(name,value){await page.getByRole('slider',{name,exact:true}).evaluate((input,value)=>{
 Object.getOwnPropertyDescriptor(HTMLInputElement.prototype,'value').set.call(input,String(value));
 input.dispatchEvent(new Event('input',{bubbles:true}));input.dispatchEvent(new Event('change',{bubbles:true}));
},value);}
const before=await probe();
await slide('Node size',1.7);await slide('Label size',17);await slide('Edge opacity',.3);
await expect.poll(async()=>(await probe()).width).toBeCloseTo(before.width*1.7,1);
await expect.poll(async()=>(await probe()).font).toBe('17px');
await expect.poll(async()=>Number((await probe()).opacity)).toBeCloseTo(.3);
await page.getByRole('combobox',{name:'Node labels',exact:true}).selectOption('none');
await expect.poll(async()=>(await probe()).label).toBe('');
await slide('Layout spacing',1.8);await slide('Force strength',18000);await slide('Link length',150);
await page.getByRole('combobox',{name:'Graph display scope'}).selectOption('all');
await slide('Node limit',60);
await expect.poll(async()=>(await probe()).ids.length).toBe(60);
await page.getByRole('button',{name:'Reset view settings'}).click();
await expect.poll(async()=>(await probe()).font).toBe('14px');
await expect.poll(async()=>(await probe()).ids.length).toBe(all.ids.length);
await page.getByRole('combobox',{name:'Graph display scope'}).selectOption('neighborhood');
await page.getByRole('combobox',{name:'Graph layout'}).selectOption('concentric');
await page.waitForTimeout(400);
await mkdir('../docs/images',{recursive:true});
await page.screenshot({path:'../docs/images/fullscreen-graph.png'});
await page.getByRole('button',{name:'Exit fullscreen graph'}).click();
await expect(page.getByRole('dialog',{name:'Graph explorer'})).toHaveCount(0);
assert.equal(await page.evaluate(()=>document.fullscreenElement),null);
await expect(page.getByRole('button',{name:'Fullscreen graph',exact:true})).toBeFocused();
assert.deepEqual(errors,[]);

// Viewport-filling fallback and Escape work when Fullscreen API access is denied.
const fallback=await browser.newPage({viewport:{width:800,height:700}});
await fallback.addInitScript(()=>{Element.prototype.requestFullscreen=()=>Promise.reject(new Error('Fixture: browser fullscreen denied'));});
await fallback.goto(base+'/?demo=1');
await fallback.getByRole('button',{name:'Fullscreen graph',exact:true}).click();
await expect(fallback.getByRole('dialog',{name:'Graph explorer'})).toBeVisible();
assert.equal(await fallback.evaluate(()=>document.fullscreenElement),null);
await fallback.keyboard.press('Escape');
await expect(fallback.getByRole('dialog',{name:'Graph explorer'})).toHaveCount(0);
assert.notEqual(await fallback.evaluate(()=>document.body.style.overflow),'hidden');
await browser.close();
console.log('PASS graph explorer: native/fullscreen fallback, Escape/focus restoration, toggle/search beyond display cap, all five layouts, live node/label/edge styles, layout controls, node limit and reset');
