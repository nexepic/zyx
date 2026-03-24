<script setup lang="ts">
import { useData } from 'vitepress'
import DefaultTheme from 'vitepress/theme'
import HomeLayout from './HomeLayout.vue'
import { computed } from 'vue'

const { frontmatter } = useData()
const isCustomHome = computed(() => frontmatter.value.customHome === true)
</script>

<template>
  <HomeLayout v-if="isCustomHome" />
  <DefaultTheme.Layout v-else />
</template>
