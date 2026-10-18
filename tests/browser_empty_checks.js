(() => {
 const checks=[];const check=(name,value)=>{if(!value)throw new Error(name);checks.push(name);};
 check('empty review initializes',!!window.axthenaReview);
 check('no invented semantic nodes',window.axthenaReview.visible.length===0);
 check('empty state is explained',document.getElementById('graph-status').textContent.includes('No eligible records'));
 document.getElementById('world').click();document.getElementById('overview').click();document.getElementById('reset').click();
 check('empty navigation remains valid',window.reviewErrors.length===0);
 check('no external resource requests',performance.getEntriesByType('resource').length===0);
 return JSON.stringify({passed:checks.length,checks},null,2);
})();
