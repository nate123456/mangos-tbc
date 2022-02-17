using System.ComponentModel.DataAnnotations.Schema;

namespace Playerbot.Api.Models;

[Table("playerbot_scripts")]
public class PlayerbotScript
{
    [Column("accountid")] public int AccountId { get; set; }
    [Column("name")] public string Name { get; set; }
    [Column("script")] public string Script { get; set; }
    [Column("data")] public string? Data { get; set; }
}
