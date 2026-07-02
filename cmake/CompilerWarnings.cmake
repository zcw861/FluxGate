# 为指定目标启用较严格但不影响跨编译器构建的告警集合。
function(fluxgate_set_warnings target_name)
    if(MSVC)
        # /W4：高等级告警；/permissive-：尽量遵循标准 C++。
        target_compile_options(${target_name} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target_name} PRIVATE
            -Wall                # 常见潜在错误。
            -Wextra              # Wall 未覆盖的额外问题。
            -Wpedantic           # 非标准语言扩展。
            -Wconversion         # 可能丢失信息的隐式数值转换。
            -Wshadow             # 局部变量遮蔽外层变量或成员。
            -Wnon-virtual-dtor   # 多态基类缺少虚析构。
            -Wold-style-cast     # C 风格强制类型转换。
        )
    endif()
endfunction()
