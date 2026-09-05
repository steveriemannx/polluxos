# patches/

对 FreeBSD base 的改动一律以 git 补丁放这里，构建时由 `scripts/apply-patches.sh`
依文件名顺序打进 `base/freebsd-src`。**不要**手改 `base/freebsd-src` 文件——
那样会让升级上游变成噩梦。

生成新补丁（在 base/freebsd-src 里改完代码后）：

```sh
cd base/freebsd-src
git add <改动文件>
git commit -m "polluxos: Xxx"
git format-patch -1 --stdout > ../../patches/0001-xxx.patch
```

约定：

- 文件名以数字开头，决定应用顺序：`0001-xxx.patch`、`0002-yyy.patch` ...
- 只相对 base 仓库根路径（`git format-patch` 默认即是）
- 已应用过的补丁再次运行脚本会自动跳过

**内部开发技巧**：改动文件当前还没稳定时，可以临时直接改 base 里的文件
（此时 apply-patches 会报 base 有未提交改动而拒绝打补丁）；稳定后再导出成
补丁，把 base 恢复干净。
