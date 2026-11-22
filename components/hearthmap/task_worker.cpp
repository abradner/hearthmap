#include "task_worker.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace esphome::hearthmap_panel {

namespace {

std::uint32_t worker_micros() {
  return static_cast<std::uint32_t>(esp_timer_get_time());
}

}  // namespace

TaskWorker::TaskWorker() : Worker(true, &worker_micros, kRestAfterUs) {}

TaskWorker::~TaskWorker() {
  if (task_ != nullptr) vTaskDelete(static_cast<TaskHandle_t>(task_));
}

bool TaskWorker::start(int core) {
  if (task_ != nullptr) return true;
  TaskHandle_t handle = nullptr;
  // Priority 1, as ESPHome's own loop: above idle, below the Wi-Fi co-processor's tasks, which
  // therefore still preempt it on its core.
  const BaseType_t ok = xTaskCreatePinnedToCore(&TaskWorker::body, "hm_worker", kStackBytes, this,
                                                1, &handle, static_cast<BaseType_t>(core));
  if (ok != pdPASS || handle == nullptr) return false;
  task_ = handle;
  core_ = core;
  return true;
}

std::uint32_t TaskWorker::stack_free() const {
  if (task_ == nullptr) return 0;
  // ESP-IDF's FreeRTOS counts stacks in bytes (StackType_t is uint8_t).
  return static_cast<std::uint32_t>(uxTaskGetStackHighWaterMark(static_cast<TaskHandle_t>(task_)) *
                                    sizeof(StackType_t));
}

void TaskWorker::body(void* self) {
  auto* w = static_cast<TaskWorker*>(self);
  for (;;) {
    // A notification given before this wait is kept, so a job queued while the last pass was
    // finishing is not lost.
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    w->run_pending();
  }
}

void TaskWorker::wake() {
  if (task_ != nullptr) xTaskNotifyGive(static_cast<TaskHandle_t>(task_));
}

void TaskWorker::rest() {
  vTaskDelay(1);
}

}  // namespace esphome::hearthmap_panel
