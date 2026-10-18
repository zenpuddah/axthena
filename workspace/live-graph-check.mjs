import {chromium,expect} from '@playwright/test';
import assert from 'node:assert/strict';
import {mkdir} from 'node:fs/promises';
import {fileURLToPath} from 'node:url';

const browser=await chromium.launch({channel:'chromium',headless:true});
const page=await browser.newPage({viewport:{width:1512,height:1000}});
const errors=[];page.on('pageerror',e=>errors.push(String(e)));
await page.goto(process.env.AXTHENA_URL??'http://127.0.0.1:17781');
await page.getByRole('button',{name:'Fullscreen graph',exact:true}).click();
await page.waitForFunction(()=>document.querySelector('.network')?._cyreg?.cy?.nodes().length>0);
// Fullscreen entry intentionally fits once; observe physics only after that transition.
await page.evaluate(()=>new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(resolve))));
const scene=()=>page.evaluate(()=>{
 const cy=document.querySelector('.network')._cyreg.cy;
 return {positions:cy.nodes().map(n=>({id:n.id(),...n.position()})),zoom:cy.zoom(),pan:cy.pan()};
});
const a=await scene();await page.waitForTimeout(300);const b=await scene();
assert.notDeepEqual(a.positions,b.positions,'Live force runs across frames without another user interaction');
assert.deepEqual(a.pan,b.pan);assert.equal(a.zoom,b.zoom);
await page.evaluate(()=>{window.originalCy=document.querySelector('.network')._cyreg.cy;window.retainedNode=window.originalCy.nodes()[0];});
await page.getByRole('button',{name:'View settings',exact:true}).click();
async function slide(name,value){await page.getByRole('slider',{name,exact:true}).evaluate((input,value)=>{
 Object.getOwnPropertyDescriptor(HTMLInputElement.prototype,'value').set.call(input,String(value));input.dispatchEvent(new Event('input',{bubbles:true}));input.dispatchEvent(new Event('change',{bubbles:true}));
},value);}
await slide('Force strength',30000);await slide('Link length',180);await slide('Center gravity',.2);
await page.waitForTimeout(300);const changed=await scene();
assert.notDeepEqual(b.positions,changed.positions);
assert.equal(changed.zoom,b.zoom);assert.deepEqual(changed.pan,b.pan);
assert.equal(await page.evaluate(()=>window.originalCy===document.querySelector('.network')._cyreg.cy),true);
await page.getByRole('button',{name:'Pause physics',exact:false}).click();
await page.waitForTimeout(100);const paused=await scene();await page.waitForTimeout(250);
assert.deepEqual((await scene()).positions,paused.positions,'Pause freezes physical positions');
await page.getByRole('button',{name:'Hide settings',exact:true}).click();
// Select and pin a rendered entity, then verify it remains fixed during real physics.
const pinId=await page.evaluate(()=>{const cy=document.querySelector('.network')._cyreg.cy;const n=cy.nodes()[0];n.emit('tap');return n.id();});
await page.getByRole('button',{name:'Pin position',exact:true}).click();
const point=await page.evaluate(id=>({...document.querySelector('.network')._cyreg.cy.getElementById(id).position()}),pinId);
await page.getByRole('button',{name:'Resume physics',exact:false}).click();
await page.waitForTimeout(350);
assert.deepEqual(await page.evaluate(id=>({...document.querySelector('.network')._cyreg.cy.getElementById(id).position()}),pinId),point);
await page.getByRole('button',{name:'Unpin position',exact:true}).click();

// Neighbors highlight on hover; remaining graph context is visibly dimmed.
const hover=await page.evaluate(()=>{
 const cy=document.querySelector('.network')._cyreg.cy,n=cy.nodes()[0];n.emit('mouseover');
 const out={neighbors:cy.elements('.neighbor').length,dimmed:cy.elements('.hover-muted').length};n.emit('mouseout');return out;
});assert.ok(hover.neighbors>1&&hover.dimmed>0);

await page.getByRole('button',{name:'Pause physics',exact:false}).click();
const old=await scene();await page.getByRole('combobox',{name:'Graph layout'}).selectOption('circle');
await page.waitForTimeout(90);const during=await scene();await page.waitForTimeout(500);const end=await scene();
assert.notDeepEqual(old.positions,during.positions);assert.notDeepEqual(during.positions,end.positions,'Layout moves through intermediate positions');
assert.equal(end.zoom,old.zoom);assert.deepEqual(end.pan,old.pan);

await page.getByRole('button',{name:'View settings',exact:true}).click();
await page.getByRole('combobox',{name:'Graph display scope'}).selectOption('all');
await page.waitForTimeout(500);
assert.equal(await page.evaluate(()=>document.querySelector('.network')._cyreg.cy===window.originalCy),true,'Expanding scope preserves renderer identity');
assert.equal(await page.evaluate(()=>window.originalCy.getElementById(window.retainedNode.id())[0]===window.retainedNode[0]),true,'Retained nodes preserve object identity');
const allCount=await page.evaluate(()=>document.querySelector('.network')._cyreg.cy.nodes().length);
await page.getByRole('region',{name:'Live graph view settings'}).getByRole('button',{name:'Activities',exact:true}).click();
await expect.poll(()=>page.evaluate(()=>document.querySelector('.network')._cyreg.cy.nodes().filter(n=>n.data('kind')==='Domain').length)).toBe(0);
await page.getByRole('region',{name:'Live graph view settings'}).getByRole('button',{name:'Activities',exact:true}).click();
await expect.poll(()=>page.evaluate(()=>document.querySelector('.network')._cyreg.cy.nodes().length)).toBe(allCount);

await page.getByRole('button',{name:'Hide settings',exact:true}).click();
await page.getByRole('button',{name:'Explore paths',exact:true}).click();
const pair=await page.evaluate(()=>{const e=document.querySelector('.network')._cyreg.cy.edges()[0];return [e.source().id(),e.target().id()];});
await page.getByRole('combobox',{name:'Path start',exact:true}).selectOption(pair[0]);
await page.getByRole('combobox',{name:'Path end',exact:true}).selectOption(pair[1]);
await expect(page.locator('.path-result')).toHaveText('1 connections');
assert.equal(await page.evaluate(()=>document.querySelector('.network')._cyreg.cy.edges('.path-active').length),1);
await page.locator('.path-steps button').last().click();
await expect(page.locator('.graph-selection')).toBeVisible();
await page.getByRole('button',{name:'Clear path highlight'}).click();
assert.equal(await page.evaluate(()=>document.querySelector('.network')._cyreg.cy.elements('.path-active').length),0);
await page.getByRole('button',{name:'Close path explorer'}).click();
// Search must reveal its target even when that entity layer was hidden.
const hidden=await page.evaluate(()=>{const n=document.querySelector('.network')._cyreg.cy.nodes().filter(n=>n.data('kind')==='Domain')[0];return {id:n.id(),label:n.data('fullLabel')};});
await page.getByRole('button',{name:'View settings',exact:true}).click();
await page.getByRole('region',{name:'Live graph view settings'}).getByRole('button',{name:'Activities',exact:true}).click();
await page.getByRole('button',{name:'Hide settings',exact:true}).click();
await page.getByRole('button',{name:'Search /',exact:true}).click();
await page.getByRole('searchbox',{name:'Search graph',exact:true}).fill(hidden.label);
await page.locator('.graph-search-results button').first().click();
await expect.poll(()=>page.evaluate(id=>document.querySelector('.network')._cyreg.cy.hasElementWithId(id),hidden.id)).toBe(true);
// Capture a fresh default view rather than a transitional stress-test layout.
await page.reload();
await page.getByRole('button',{name:'Fullscreen graph',exact:true}).click();
await page.getByRole('button',{name:'View settings',exact:true}).click();
await page.waitForTimeout(1500);
await page.getByRole('button',{name:'Fit',exact:true}).click();
await page.waitForTimeout(100);
const images=new URL('../docs/images/',import.meta.url);
await mkdir(images,{recursive:true});await page.screenshot({path:fileURLToPath(new URL('live-graph.png',images))});
console.log('Simulation indicator:',await page.locator('.simulation-status').textContent());
assert.deepEqual(errors,[]);await browser.close();
console.log('PASS live scene: continuous movement, next-frame parameter effects, stable camera/renderer/node identity, pause, pins, hover neighbors, animated transitions, layer filtering and shortest-path highlighting');
