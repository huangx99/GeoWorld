# persistence

状态：M5-A 前半已完成——LSN/BranchId/错误码契约、单 writer WAL（有界队列、组提交、segment rotation、CRC32C、只读故障态）、WAL reader（断电尾部修剪、损坏 fail-closed）、本地文件系统存储抽象与原子发布、目录 manifest 均已落地并有单元测试；检查点 provider、恢复编排、回放与分支属于 M5-B 及以后批次。
