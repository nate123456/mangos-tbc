using System.ComponentModel.DataAnnotations.Schema;

namespace Playerbot.Core.Models;

[Table("playerbot_scripts")]
public class PlayerbotScript
{
    [Column("accountid")] public int AccountId { get; set; } = 0;
    [Column("name")] public string Name { get; set; } = string.Empty;
    [Column("script")] public string Script { get; set; } = string.Empty;
    [Column("data")] public string? Data { get; set; }
}
