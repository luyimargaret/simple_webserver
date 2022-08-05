# simple_webserver
- 使用 **线程池 + epoll(ET模式) + Proactor事件处理模式** 的并发模型
- 使用**状态机**解析HTTP请求报文，支持解析**GET请求，可以请求服务器的图片和视频文件**
- 经Webbench压力测试可以实现**上万的并发连接**数据交换
