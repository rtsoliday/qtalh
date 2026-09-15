<script setup lang="ts">
import DefaultTheme from 'vitepress/theme'
import { withBase } from 'vitepress'

const { Layout } = DefaultTheme
</script>

<template>
  <Layout>
    <template #doc-before>
      <nav class="doc-affiliation" aria-label="Institutional links">
        <a href="https://www.aps.anl.gov/">Advanced Photon Source</a>
        <span aria-hidden="true">·</span>
        <a href="https://www.anl.gov/">Argonne National Laboratory</a>
      </nav>
    </template>
    <template #doc-after>
      <footer class="doc-institutional-footer" aria-label="Documentation credits">
        <p class="doc-institutional-links">
          <a href="https://www.aps.anl.gov/">Advanced Photon Source</a>
          <span aria-hidden="true">·</span>
          <a href="https://www.anl.gov/">Argonne National Laboratory</a>
        </p>
        <p>Qt port development and maintenance:
          <a :href="withBase('/project/authors.html')">Robert Soliday</a>.
        </p>
        <p>Based on EPICS ALH 1.2.35, developed at Argonne National Laboratory.
          <a :href="withBase('/project/authors.html')">Original credits</a>
          <span aria-hidden="true"> · </span>
          <a :href="withBase('/project/license.html')">License</a>.
        </p>
      </footer>
    </template>
  </Layout>
</template>
