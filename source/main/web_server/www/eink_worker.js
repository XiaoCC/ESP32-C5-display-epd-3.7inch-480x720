/* 复用主线程的六色算法，避免大图转换阻塞管理页。 */
importScripts('/eink_pipeline.js');
self.onmessage = ({data}) => {
  try {
    EinkPipeline.setPanelRotation(data.rotation);
    const steps = EinkPipeline.quantizeSteps(data.rgba, data.settings);
    for (;;) {
      const step = steps.next();
      if (step.done) {
        const {ids, preview} = step.value;
        self.postMessage({ids, preview}, [ids.buffer, preview.buffer]);
        return;
      }
      self.postMessage({progress:step.value});
    }
  } catch (error) {
    self.postMessage({error:error.message});
  }
};
