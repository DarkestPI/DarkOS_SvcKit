#!/usr/bin/env python3
"""
C++项目初始化脚手架脚本
"""
import os
import re
import shutil
import argparse
from pathlib import Path

TEMPLATE_DIR = Path("./SvcKit/svckit_project_template")
PLACEHOLDER = "${PROJECT_NAME}"

def replace_in_file(filepath, old, new):
    """替换文件中的占位符"""
    with open(filepath, 'r', encoding='utf-8') as f:
        content = f.read()
    content = content.replace(old, new)
    with open(filepath, 'w', encoding='utf-8') as f:
        f.write(content)

def main():
    parser = argparse.ArgumentParser(description='生成新的C++项目')
    parser.add_argument('project_name', help='项目名称（英文，建议小写+下划线）')
    parser.add_argument('--output', '-o', default='.', help='输出目录')
    args = parser.parse_args()

    project_name = args.project_name
    target_dir = Path(args.output) / project_name

    if target_dir.exists():
        print(f"错误：目录 {target_dir} 已存在！")
        return

    # 1. 复制模板
    print(f"正在创建项目: {project_name}")
    shutil.copytree(TEMPLATE_DIR, target_dir)

    # 2. 重命名占位符目录
    placeholder_dir = target_dir / "include" / PLACEHOLDER
    if placeholder_dir.exists():
        new_dir = target_dir / "include" / project_name
        shutil.move(placeholder_dir, new_dir)

    # 3. 替换文件内容中的占位符
    for filepath in target_dir.rglob("*"):
        if filepath.is_file():
            try:
                replace_in_file(filepath, PLACEHOLDER, project_name)
                replace_in_file(filepath, PLACEHOLDER.upper(), project_name.upper())
            except UnicodeDecodeError:
                # 忽略二进制文件
                pass

    # 4. 重命名特定文件（可选）
    cmake_file = target_dir / "CMakeLists.txt"
    if cmake_file.exists():
        # 确保CMake中的项目名被正确设置
        replace_in_file(cmake_file, "project(TEMPLATE_PROJECT)", f"project({project_name})")

    print(f"项目创建成功！目录: {target_dir.absolute()}")
    print("下一步:")
    print(f"  cd {target_dir}")
    print("  mkdir build && cd build")
    print("  cmake .. -DCMAKE_BUILD_TYPE=Release")
    print("  cmake --build .")

if __name__ == "__main__":
    main()